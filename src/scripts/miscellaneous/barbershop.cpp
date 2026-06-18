#include "scriptPCH.h"
#include "Barbershop/TurtleBarbershopMgr.h"

bool GossipHello_npc_surgeon_go(Player* pPlayer, Creature* pCreature)
{
    if (!pPlayer || !pCreature)
        return false;

    pPlayer->CLOSE_GOSSIP_MENU();
    sTurtleBarbershopMgr.OpenBarbershop(pPlayer);
    return true;
}

void AddSC_barbershop()
{
    Script* newscript = new Script;
    newscript->Name = "npc_surgeon_go";
    newscript->pGossipHello = &GossipHello_npc_surgeon_go;
    newscript->RegisterSelf();
}