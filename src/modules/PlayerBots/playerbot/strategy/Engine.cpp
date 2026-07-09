
#include "playerbot/playerbot.h"
#include <stdarg.h>
#include <iomanip>

#include "Engine.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PerformanceMonitor.h"
#include "playerbot/BotLearningMgr.h"
#include "playerbot/BotActionLog.h"
#include "MapManager.h"
#include <algorithm>

#ifdef BUILD_ELUNA
#include "LuaEngine/LuaEngine.h"
#endif

using namespace ai;

Engine::Engine(PlayerbotAI* ai, AiObjectContext *factory, BotState state) : PlayerbotAIAware(ai), aiObjectContext(factory), state(state)
{
    lastRelevance = 0.0f;
    testMode = false;
    lastExecutedAction = nullptr;
}

bool ActionExecutionListeners::Before(Action* action, const Event& event)
{
    bool result = true;
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i!=listeners.end(); i++)
    {
        result &= (*i)->Before(action, event);
    }
    return result;
}

void ActionExecutionListeners::After(Action* action, bool executed, const Event& event)
{
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i!=listeners.end(); i++)
    {
        (*i)->After(action, executed, event);
    }
}

bool ActionExecutionListeners::OverrideResult(Action* action, bool executed, const Event& event)
{
    bool result = executed;
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i!=listeners.end(); i++)
    {
        result = (*i)->OverrideResult(action, result, event);
    }
    return result;
}

bool ActionExecutionListeners::AllowExecution(Action* action, const Event& event)
{
    bool result = true;
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i!=listeners.end(); i++)
    {
        result &= (*i)->AllowExecution(action, event);
    }
    return result;
}

ActionExecutionListeners::~ActionExecutionListeners()
{
    for (std::list<ActionExecutionListener*>::iterator i = listeners.begin(); i!=listeners.end(); i++)
    {
        delete *i;
    }
    listeners.clear();
}


Engine::~Engine(void)
{
    Reset();

    strategies.clear();
}

void Engine::Reset()
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    ActionNode* action = NULL;
    do
    {
        action = queue.Pop();
        if (!action) break;
        delete action;
    } while (true);

    for (std::list<TriggerNode*>::iterator i = triggers.begin(); i != triggers.end(); i++)
    {
        TriggerNode* trigger = *i;
        delete trigger;
    }
    triggers.clear();

    for (std::list<Multiplier*>::iterator i = multipliers.begin(); i != multipliers.end(); i++)
    {
        Multiplier* multiplier = *i;
        delete multiplier;
    }
    multipliers.clear();
}

void Engine::Init()
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    Reset();

    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        Strategy* strategy = i->second;
        strategy->InitMultipliers(multipliers, state);
        strategy->InitTriggers(triggers, state);
        MultiplyAndPush(strategy->getDefaultActions(state), 0.0f, false, Event(), "default");
    }

	if (testMode)
	{
        FILE* file = fopen("test.log", "w");
        fprintf(file, "\n");
        fclose(file);
	}
}

bool Engine::DoNextAction(Unit* unit, int depth, bool minimal, bool isStunned)
{
    MapManager::SetContinentUpdatePhase("engine-lock", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
    std::unique_lock<std::recursive_mutex> lock(engineMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        MapManager::SetContinentUpdatePhase("engine-busy", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
        return false;
    }
    MapManager::SetContinentUpdatePhase("engine-entry", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
    LogAction("--- AI Tick ---");
    if (sPlayerbotAIConfig.logValuesPerTick)
    {
        MapManager::SetContinentUpdatePhase("engine-log-values", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
        LogValues();
    }

    bool actionExecuted = false;
    ActionBasket* basket = NULL;

    time_t currentTime = time(0);
    MapManager::SetContinentUpdatePhase("engine-context", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
    aiObjectContext->Update();
    // Cache the outnumbered state once per tick for the learned-policy lookup below.
    if (sPlayerbotAIConfig.learnedPolicyEnabled)
        policyAttackerBucket = (aiObjectContext->GetValue<uint8>("attackers count")->Get() >= 2) ? 1 : 0;
    MapManager::SetContinentUpdatePhase("engine-triggers", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
    ProcessTriggers(minimal);
    MapManager::SetContinentUpdatePhase("engine-defaults", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
    PushDefaultActions();

    std::vector<Action*> modifiedActions;

    int iterations = 0;
    uint32 iterationScale = minimal ? std::max<uint32>(1, sPlayerbotAIConfig.iterationsPerTick / 2) : sPlayerbotAIConfig.iterationsPerTick;
    uint32 iterationsPerTick = std::min<uint32>(queue.Size() * iterationScale, minimal ? 100u : 250u);
    do 
    {
        basket = queue.Peek();
        if (basket) 
        {
            float relevance = basket->getRelevance(), oldRelevance = relevance; // just for reference
            bool skipPrerequisites = basket->isSkipPrerequisites();
            Event event = basket->getEvent();
            if (minimal && (relevance < 100))
            {
                ActionNode* lowPriority = queue.Pop(basket);
                if (lowPriority)
                    delete lowPriority;
                continue;
            }
            // NOTE: queue.Pop() deletes basket
            ActionNode* actionNode = queue.Pop();
            Action* action = InitializeAction(actionNode);

            std::string actionName = (action ? action->getName() : "unknown");
            if (!event.getSource().empty())
                actionName += " <" + event.getSource() + ">";
            
            auto pmo1 = sPerformanceMonitor.start(PERF_MON_ACTION, actionName, ai);

            if(action)
                action->setRelevance(relevance);

            if (!action)
            {
                if (sPlayerbotAIConfig.CanLogAction(ai, actionNode->getName(), false, ""))
                {
                    std::ostringstream out;
                    out << "try: ";
                    out << actionNode->getName();
                    out << " unknown (";

                    out << std::fixed << std::setprecision(3);
                    out << relevance << ")";

                    if (!event.getSource().empty())
                        out << " [" << event.getSource() << "]";

                    if (ai->GetMaster())
                    {
                        ai->TellPlayerNoFacing(ai->GetMaster(), out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
                    }
                    else
                    {
                        ai->GetBot()->Say(out.str(), PlayerbotChatLanguage(ai->GetBot()));
                    }
                }
                LogAction("A:%s - UNKNOWN", actionNode->getName().c_str());
            }
            else
            {
                bool isUseful = false;
                if (!isStunned || action->isUsefulWhenStunned())
                {
                    MapManager::SetContinentUpdatePhaseNamed("engine-useful", action ? action->getName() : "unknown", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
                    auto pmo2 = sPerformanceMonitor.start(PERF_MON_ACTION, "isUseful", ai);
                    isUseful = action->isUseful();
                    pmo2.reset();
                }

                if (isUseful)
                {
                    if (std::find(modifiedActions.begin(), modifiedActions.end(), action) == modifiedActions.end())
                    {
                        for (std::list<Multiplier*>::iterator i = multipliers.begin(); i != multipliers.end(); i++)
                        {
                            Multiplier* multiplier = *i;
                            relevance *= multiplier->GetValue(action);

                            action->setRelevance(relevance);
                            if (!relevance)
                            {
                                LogAction("Multiplier %s made action %s useless", multiplier->getName().c_str(), action->getName().c_str());
                                break;
                            }
                        }
                    }

                    ActionBasket* peekAction = queue.Peek();
                    if (relevance < oldRelevance && peekAction && peekAction->getRelevance() > relevance) //Relevance changed. Try again.
                    {
                        modifiedActions.push_back(action);
                        PushAgain(actionNode, relevance, event);
                        continue;
                    }

                    if (!skipPrerequisites)
                    {
                        LogAction("A:%s - PREREQ", action->getName().c_str());
                        if (MultiplyAndPush(actionNode->getPrerequisites(), relevance + 0.02, false, event, "prereq"))
                        {
                            PushAgain(actionNode, relevance + 0.01, event);
                            continue;
                        }
                    }

                    auto pmo3 = sPerformanceMonitor.start(PERF_MON_ACTION, "isPossible", ai);
                    MapManager::SetContinentUpdatePhase("engine-possible", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
                    bool isPossible = action->isPossible();
                    pmo3.reset();

                    if (isPossible && relevance)
                    {
                        MapManager::SetContinentUpdatePhase("engine-execute", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
                        auto pmo4 = sPerformanceMonitor.start(PERF_MON_ACTION, "Execute", ai);
                        actionExecuted = ListenAndExecute(action, event);
                        pmo4.reset();

#ifdef PLAYERBOT_ELUNA
                        // used by eluna    
                        if (Eluna* e = ai->GetBot()->GetEluna())
                            e->OnActionExecute(ai, action->getName(), actionExecuted);
#endif

                        if (actionExecuted)
                        {
                            // TEMP BGENGINE TRACE: the mock games show statues whose tactics
                            // actions never run -- log the REAL per-tick winner for BG bots.
                            if (ai->GetBot() && ai->GetBot()->InBattleGround())
                            {
                                static std::mutex beMx;
                                static std::unordered_map<uint32, uint32> beLast;
                                const uint32 beNow = WorldTimer::getMSTime();
                                std::lock_guard<std::mutex> beLk(beMx);
                                uint32& t = beLast[ai->GetBot()->GetGUIDLow()];
                                if (!t || beNow - t >= 10000)
                                {
                                    t = beNow;
                                    sLog.outString("BGENGINE %s won=%s rel=%.1f", ai->GetBot()->GetName(), action->getName().c_str(), relevance);
                                }
                            }
                            LogAction("A:%s - OK", action->getName().c_str());
                            MultiplyAndPush(actionNode->getContinuers(), 0, false, event, "cont");
                            lastRelevance = relevance;
                            delete actionNode;
                            break;
                        }
                        else
                        {
                            LogAction("A:%s - FAILED", action->getName().c_str());
                            MultiplyAndPush(actionNode->getAlternatives(), relevance + 0.03, false, event, "alt");
                        }
                    }
                    else
                    {
                        if (sPlayerbotAIConfig.CanLogAction(ai,actionNode->getName(), false, ""))
                        {
                            std::ostringstream out;
                            out << "try: ";
                            out << action->getName();
                            out << " impossible (";

                            out << std::fixed << std::setprecision(3);
                            out << action->getRelevance() << ")";

                            if (!event.getSource().empty())
                                out << " [" << event.getSource() << "]";

        if (ai->GetMaster())
                            {
                                ai->TellPlayerNoFacing(ai->GetMaster(), out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
                            }
                            else
                            {
                                ai->GetBot()->Say(out.str(), PlayerbotChatLanguage(ai->GetBot()));
                            }
                        }
                        LogAction("A:%s - IMPOSSIBLE", action->getName().c_str());
                        MultiplyAndPush(actionNode->getAlternatives(), relevance + 0.03, false, event, "alt");
                    }
                }
                else
                {
                    if (sPlayerbotAIConfig.CanLogAction(ai,actionNode->getName(), false, ""))
                    {
                        std::ostringstream out;
                        out << "try: ";
                        out << action->getName();
                        out << " useless (";

                        out << std::fixed << std::setprecision(3);
                        out << action->getRelevance() << ")";

                        if (!event.getSource().empty())
                            out << " [" << event.getSource() << "]";

        if (ai->GetMaster())
                        {
                            ai->TellPlayerNoFacing(ai->GetMaster(), out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
                        }
                        else
                        {
                            ai->GetBot()->Say(out.str(), PlayerbotChatLanguage(ai->GetBot()));
                        }
                    }
                    lastRelevance = relevance;
                    LogAction("A:%s - USELESS", action->getName().c_str());
                }
            }
            delete actionNode;
        }
    }
    while (basket && ++iterations <= iterationsPerTick);

    /*
    if (!basket)
    {
        lastRelevance = 0.0f;
        PushDefaultActions();
        if (queue.Peek() && depth < 2)
            return DoNextAction(unit, depth + 1, minimal, isStunned);
    }
    */

    // MEMORY FIX TEST
 /*   do {
        basket = queue.Peek();
        if (basket) {
            // NOTE: queue.Pop() deletes basket
            delete queue.Pop();
        }
    } while (basket);*/

    if (time(0) - currentTime > 1) {
        LogAction("too long execution");
    }

    if (!actionExecuted)
        LogAction("no actions executed");

    queue.RemoveExpired();
    return actionExecuted;
}

ActionNode* Engine::CreateActionNode(const std::string& name)
{
    ActionNode* actionNode = nullptr;
    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        Strategy* strategy = i->second;
        actionNode = strategy->GetAction(name);
        if (actionNode)
        {
            break;
        }
    }

    if (!actionNode)
    {
        actionNode = new ActionNode(name);
    }

    return actionNode;
}

bool Engine::MultiplyAndPush(NextAction** actions, float forceRelevance, bool skipPrerequisites, const Event& event, const char* pushType)
{
    bool pushed = false;
    if (actions)
    {
        for (int j=0; actions[j]; j++)
        {
            NextAction* nextAction = actions[j];
            if (nextAction)
            {
                ActionNode* actionNode = CreateActionNode(nextAction->getName());
                InitializeAction(actionNode);

                bool shouldPush = false;
                float k = nextAction->getRelevance();
                if (forceRelevance > 0.0f)
                {
                    k = forceRelevance;
                }
                else if (strcmp(pushType, "default") == 0)
                {
                    k -= 200.0f;
                    shouldPush = true;
                }
                else if (strcmp(pushType, "prereq") == 0 || strcmp(pushType, "alt") == 0 || strcmp(pushType, "again") == 0)
                {
                    k = forceRelevance;
                    shouldPush = true;
                }

                if (!shouldPush)
                {
                    shouldPush = k > 0.0f;
                }

                // LEARNED POLICY: bias triggered actions toward what historically wins
                // for this bot's class + current state. GetActionRelevanceBonus is bounded
                // (+/-8) and scaled by the bot's experiment cohort strength (0 = control),
                // so it nudges the rotation without overriding hand-coded priorities or
                // emergencies (k>=90). This closes the loop: logged (state,action,reward)
                // -> learned action values -> the engine actually acts on them.
                if (shouldPush && k > 0.0f && k < 90.0f && std::strcmp(pushType, "trigger") == 0 &&
                    sPlayerbotAIConfig.learnedPolicyEnabled && ai && ai->GetBot())
                {
                    Player* pbot = ai->GetBot();
                    k += BotExperiment::PolicyStrength(pbot->GetGUIDLow()) *
                         sBotLearningMgr.GetActionRelevanceBonus(pbot, policyAttackerBucket, actionNode->getName());
                }

                if (shouldPush)
                {
                    LogAction("PUSH:%s - %f (%s)", actionNode->getName().c_str(), k, pushType);
                    queue.Push(new ActionBasket(actionNode, k, skipPrerequisites, event));
                    pushed = true;
                }
                else
                {
                    delete actionNode;
                }

                delete nextAction;
            }
            else
                break;
        }
        delete[] actions;
    }
    return pushed;
}

ActionResult Engine::ExecuteAction(const std::string& name, Event& event)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    ActionResult actionResult = ACTION_RESULT_UNKNOWN;
    ActionNode* actionNode = CreateActionNode(name);
    if (actionNode)
    {
        auto pmo1 = sPerformanceMonitor.start(PERF_MON_ACTION, name, ai);
        Action* action = InitializeAction(actionNode);
        if (action)
        {
            auto pmo2 = sPerformanceMonitor.start(PERF_MON_ACTION, "isUseful", ai);
            bool isUseful = action->isUseful();
            pmo2.reset();
            
            if (isUseful)
            {
                auto pmo3 = sPerformanceMonitor.start(PERF_MON_ACTION, "isPossible", ai);
                bool isPossible = action->isPossible();
                pmo3.reset();

                if (isPossible)
                {
                    action->MakeVerbose(event.getOwner() != nullptr);
                    auto pmo4 = sPerformanceMonitor.start(PERF_MON_ACTION, "Execute", ai);
                    bool executionResult = ListenAndExecute(action, event);
                    pmo4.reset();

                    MultiplyAndPush(action->getContinuers(), 0.0f, false, event, "default");
                    actionResult = executionResult ? ACTION_RESULT_OK : ACTION_RESULT_FAILED;
                }
                else
                {
                    actionResult = ACTION_RESULT_IMPOSSIBLE;
                }
            }
            else
            {
                actionResult = ACTION_RESULT_USELESS;
            }
        }
        delete actionNode;
    }

    return actionResult;
}

bool Engine::CanExecuteAction(const std::string& name, bool isUseful, bool isPossible)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    bool result = true;
    ActionNode* actionNode = CreateActionNode(name);
    if (actionNode)
    {
        Action* action = InitializeAction(actionNode);
        if (action)
        {
            if (isUseful)
            {
                result &= action->isUseful();
            }

            if (isPossible)
            {
                result &= action->isPossible();
            }
        }

        delete actionNode;
    }

    return result;
}

void Engine::addStrategy(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    removeStrategy(name, initMode);

    Strategy* strategy = aiObjectContext->GetStrategy(name);
    if (strategy)
    {
        std::set<std::string> siblings = aiObjectContext->GetSiblingStrategy(name);
        for (std::set<std::string>::iterator i = siblings.begin(); i != siblings.end(); i++)
        {
            removeStrategy(*i, initMode);
        }

        LogAction("S:+%s", strategy->getName().c_str());
        strategies[strategy->getName()] = strategy;
        strategy->OnStrategyAdded(state);
    }

    if(!initMode)
    {
        Init();
    }
}

void Engine::addStrategies(std::string first, ...)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
	addStrategy(first);

	va_list vl;
	va_start(vl, first);

	const char* cur;
	do
	{
		cur = va_arg(vl, const char*);
		if (cur)
			addStrategy(cur);
	}
	while (cur);

	va_end(vl);
}

bool Engine::removeStrategy(const std::string& name, bool init)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    std::map<std::string, Strategy*>::iterator i = strategies.find(name);
    if (i == strategies.end())
        return false;

    LogAction("S:-%s", name.c_str());
    i->second->OnStrategyRemoved(state);
    strategies.erase(i);

    if (init)
    {
        Init();
    }
    
    return true;
}

void Engine::removeAllStrategies()
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    strategies.clear();
    Init();
}

void Engine::toggleStrategy(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    if (!removeStrategy(name))
        addStrategy(name);
}

bool Engine::HasStrategy(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    return strategies.find(name) != strategies.end();
}

Strategy* Engine::GetStrategy(const std::string& name) const
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    auto i = strategies.find(name);
    if (i != strategies.end())
    {
        return i->second;
    }

    return nullptr;
}

void Engine::ProcessTriggers(bool minimal)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    std::vector<TriggerNode*> triggerSnapshot(triggers.begin(), triggers.end());

    for (TriggerNode* node : triggerSnapshot)
    {
        if (!node)
            continue;

        Trigger* trigger = node->getTrigger();
        if (!trigger)
        {
            trigger = aiObjectContext->GetTrigger(node->getName());
            node->setTrigger(trigger);
        }
        if (!trigger)
            continue;

        if (testMode || trigger->IsAlreadyTriggered() || trigger->needCheck())
        {
            auto pmo = sPerformanceMonitor.start(PERF_MON_TRIGGER, trigger->getName(), ai);
            MapManager::SetContinentUpdatePhaseNamed("trigger-check", trigger->getName(), ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
            Event event = trigger->Check();

#ifdef PLAYERBOT_ELUNA
            // used by eluna    
            if (Eluna* e = ai->GetBot()->GetEluna())
                e->OnTriggerCheck(ai, trigger->getName(), !event ? false : true);
#endif

            if (!event)
                continue;

            MapManager::SetContinentUpdatePhase("trigger-handlers", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
            MultiplyAndPush(node->getHandlers(), 0.0f, false, event, "trigger");
            LogAction("T:%s", trigger->getName().c_str());
        }
    }

    for (TriggerNode* node : triggerSnapshot)
    {
        if (!node)
            continue;

        Trigger* trigger = node->getTrigger();
        if (trigger) trigger->Reset();
    }
}

void Engine::PushDefaultActions()
{
    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        Strategy* strategy = i->second;
        MultiplyAndPush(strategy->getDefaultActions(state), 0.0f, false, Event(), "default");
    }
}

std::string Engine::ListStrategies()
{   
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    std::string s;
    if (strategies.empty())
        return s;

    for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
    {
        s.append(i->first);
        s.append(", ");
    }
    return s.substr(0, s.length() - 2);
}

std::list<std::string_view> Engine::GetStrategies()
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    std::list<std::string_view> result;
    for (const auto& strategy : strategies)
    {
        result.push_back(strategy.first);
    }
    return result;
}

void Engine::PushAgain(ActionNode* actionNode, float relevance, const Event& event)
{
    NextAction** nextAction = new NextAction*[2];
    nextAction[0] = new NextAction(actionNode->getName(), relevance);
    nextAction[1] = NULL;
    MultiplyAndPush(nextAction, relevance, true, event, "again");
    delete actionNode;
}

bool Engine::ContainsStrategy(StrategyType type)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
	for (std::map<std::string, Strategy*>::iterator i = strategies.begin(); i != strategies.end(); i++)
	{
		Strategy* strategy = i->second;
		if (strategy->GetType() & type)
			return true;
	}
	return false;
}

Action* Engine::InitializeAction(ActionNode* actionNode)
{
    Action* action = actionNode->getAction();
    if (!action)
    {
        action = aiObjectContext->GetAction(actionNode->getName());
        actionNode->setAction(action);
    }

    if (action)
    {
        action->SetReaction(false);
    }

    return action;
}

bool Engine::ListenAndExecute(Action* action, Event& event)
{
    bool actionExecuted = false;
    Action* prevExecutedAction = lastExecutedAction;
    MapManager::SetContinentUpdatePhase("action-before", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
    if (actionExecutionListeners.Before(action, event))
    {
        ai->SetLastEvent(event);
        MapManager::SetContinentUpdatePhaseNamed("action-allow", action ? action->getName() : "unknown", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
        if (actionExecutionListeners.AllowExecution(action, event))
        {
            MapManager::SetContinentUpdatePhaseNamed("action-execute", action ? action->getName() : "unknown", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
            actionExecuted = action->Execute(event);
        }
        else
        {
            actionExecuted = true;
        }
        MapManager::SetContinentUpdatePhase("action-done", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
        if (actionExecuted)
        {
            ai->SetActionDuration(action);
            lastExecutedAction = action;
        }
    }

    MapManager::SetContinentUpdatePhase("action-log", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
    std::string lastActionName = prevExecutedAction ? prevExecutedAction->getName() : "";
    if (sPlayerbotAIConfig.CanLogAction(ai, action->getName(), true, lastActionName))
    {
        std::ostringstream out;
        out << "do: ";
        out << action->getName();
        if (actionExecuted)
            out << " 1 (";
        else
            out << " 0 (";

        out << std::fixed << std::setprecision(2);
        out << action->getRelevance() << ")";

        if(!event.getSource().empty())
            out << " [" << event.getSource() << "]";

        if (actionExecuted)
        {
            const uint32 actionDuration = action->GetDuration();
            if (actionDuration > 0)
            {
                out << " (duration: " << ((float)actionDuration / static_cast<float>(IN_MILLISECONDS)) << "s)";
            }
        }

        if (ai->GetMaster())
        {
            ai->TellPlayerNoFacing(ai->GetMaster(), out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
        }
        else
        {
            ai->GetBot()->Say(out.str(), PlayerbotChatLanguage(ai->GetBot()));
        }
    }

    if (ai->HasStrategy("debug threat", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        AiObjectContext* context = ai->GetAiObjectContext();

        float deltaThreat = LOG_AI_VALUE(float, "my threat::current target")->GetDelta(5.0f);

        float currentThreat = AI_VALUE2(float, "my threat", "current target");
        float tankThreat = AI_VALUE2(float, "tank threat", "current target");
        float relThreat = AI_VALUE2(uint8, "threat", "current target");

        out << "threat: " << int32(currentThreat)<< "+" << int32(deltaThreat) << " / " << int32(tankThreat) << " ||| " << relThreat;

        ai->TellPlayerNoFacing(ai->GetMaster(), out);
    }

    MapManager::SetContinentUpdatePhase("action-override", ai && ai->GetBot() ? ai->GetBot()->GetGUIDLow() : 0);
    actionExecuted = actionExecutionListeners.OverrideResult(action, actionExecuted, event);
    actionExecutionListeners.After(action, actionExecuted, event);
    return actionExecuted;
}

void Engine::LogAction(const char* format, ...)
{
    char buf[1024];

    va_list ap;
    va_start(ap, format);
    vsprintf(buf, format, ap);
    va_end(ap);
    lastAction += "|";
    lastAction += buf;
    if (lastAction.size() > 512)
    {
        lastAction = lastAction.substr(512);
        size_t pos = lastAction.find("|");
        lastAction = (pos == std::string::npos ? "" : lastAction.substr(pos));
    }

    if (testMode)
    {
        FILE* file = fopen("test.log", "a");
        fprintf(file, "%s",buf);
        fprintf(file, "\n");
        fclose(file);
    }
    else
    {
        Player* bot = ai->GetBot();

        // BotActionLog tee: every PUSH/A/Tick line also lands in the bot's
        // per-bot file under logs/bots/ when AiPlayerbot.EnableActionLog=1.
        // This MUST run regardless of group status - the logInGroupOnly guard
        // below only limits console (info.log) spam, not the per-bot trace, and
        // gating the tee on it meant solo bots logged no decisions at all.
        // Tag heuristic extracts the first colon-prefix from `buf` so the
        // per-bot log gets useful tags (PUSH / A / T / etc.) instead of
        // a single "ACTION".
        const char* tag = "ENGINE";
        if (strncmp(buf, "PUSH:", 5) == 0)             tag = "PUSH";
        else if (strncmp(buf, "A:", 2) == 0)           tag = "ACTION";
        else if (strncmp(buf, "T:", 2) == 0)           tag = "TRIGGER_REASON";
        else if (strncmp(buf, "--- AI Tick", 11) == 0) tag = "TICK";
        else if (strncmp(buf, "no actions", 10) == 0)  tag = "NO_ACTION";
        ai::botdiag::BotActionLog::Write(ai, tag, "%s", buf);

        if (sPlayerbotAIConfig.logInGroupOnly && !bot->GetGroup())
            return;

        sLog.outDetail( "%s %s", bot->GetName(), buf);
    }
}

void Engine::ChangeStrategy(const std::string& names)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    std::vector<std::string> splitted = split(names, ',');
    for (std::vector<std::string>::iterator i = splitted.begin(); i != splitted.end(); i++)
    {
        const char* name = i->c_str();
        switch (name[0])
        {
            case '+':
            {
                addStrategy(name+1);
                break;
            }
            case '-':
            {
                removeStrategy(name+1);
                break;
            }
            case '~':
            {
                toggleStrategy(name+1);
                break;
            }
        }
    }
}

void Engine::PrintStrategies(Player* requester, const std::string& engineType)
{
    std::lock_guard<std::recursive_mutex> lock(engineMutex);
    std::string engineStrategies = engineType;
    engineStrategies.append(" Strategies: ");
    engineStrategies.append(ListStrategies());
    ai->TellPlayer(requester, engineStrategies, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, true);
}

void Engine::LogValues()
{
    if (testMode)
        return;

    Player* bot = ai->GetBot();
    if (sPlayerbotAIConfig.logInGroupOnly && !bot->GetGroup())
        return;

    std::string text = ai->GetAiObjectContext()->FormatValues();
    sLog.outDebug( "Values for %s: %s", bot->GetName(), text.c_str());
}
