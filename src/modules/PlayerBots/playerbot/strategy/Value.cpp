
#include "playerbot/playerbot.h"
#include "Value.h"
#include "playerbot/PerformanceMonitor.h"
#include "playerbot/ChatHelper.h"

using namespace ai;

std::string ObjectGuidCalculatedValue::Format()
{
    GuidPosition guid = GuidPosition(this->Calculate(), bot);
    return guid ? chat->formatGuidPosition(guid, bot) : "<none>";
}

std::string ObjectGuidListCalculatedValue::Format()
{
    std::ostringstream out; out << "{";
    std::list<ObjectGuid> guids = this->Calculate();
    for (std::list<ObjectGuid>::iterator i = guids.begin(); i != guids.end(); ++i)
    {
        GuidPosition guid = GuidPosition(*i, bot);
        out << chat->formatGuidPosition(guid,bot) << ",";
    }
    out << "}";
    return out.str();
}

std::string GuidPositionCalculatedValue::Format()
{
    std::ostringstream out;
    GuidPosition guidP = this->Calculate();
    return chat->formatGuidPosition(guidP,bot);
}

std::string GuidPositionListCalculatedValue::Format()
{
    std::ostringstream out; out << "{";
    std::list<GuidPosition> guids = this->Calculate();
    for (std::list<GuidPosition>::iterator i = guids.begin(); i != guids.end(); ++i)
    {
        GuidPosition guidP = *i;
        out << chat->formatGuidPosition(guidP,bot) << ",";
    }
    out << "}";
    return out.str();
}

std::string GuidPositionManualSetValue::Format()
{
    return chat->formatGuidPosition(value,bot);
}

// DANGLING-POINTER FIX (core-dump proven): a raw Unit* cached across ticks dangles when the unit is
// deleted inside the cache window (2s for "grind target") -- the vtable call in
// AttackAnythingAction::isUseful() then jumped to freed heap (SIGSEGV, ~4/hr once attack-anything ran
// hot). NEVER return the raw cached pointer: remember the GUID at compute time and RE-RESOLVE it on
// every cached read; a despawned/deleted unit resolves to nullptr instead of a landmine. Fresh
// Calculate() results are safe for the current tick by construction.
Unit* UnitCalculatedValue::Get()
{
    std::lock_guard<std::recursive_mutex> lock(valueMutex);
    time_t now = time(0);
    const bool stale = !lastCheckTime ||
        (checkInterval < 2 && (now - lastCheckTime > 0.1)) ||
        now - lastCheckTime >= checkInterval / 2;
    if (stale)
    {
        Unit* fresh = CalculatedValue<Unit*>::Get();   // recursive mutex -> safe re-entry
        cachedGuid = fresh ? fresh->GetObjectGuid() : ObjectGuid();
        return fresh;
    }
    if (!cachedGuid)
        return nullptr;
    return ai->GetUnit(cachedGuid);                    // nullptr if it no longer exists
}

void UnitCalculatedValue::Set(Unit* val)
{
    std::lock_guard<std::recursive_mutex> lock(valueMutex);
    cachedGuid = val ? val->GetObjectGuid() : ObjectGuid();
    CalculatedValue<Unit*>::Set(val);
}
