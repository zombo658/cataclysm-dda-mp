#pragma once
#ifndef CATA_SRC_MP_REMOTE_TRADE_H
#define CATA_SRC_MP_REMOTE_TRADE_H

#include <string>

class Character;
class JsonObject;
class npc;

// Trading for the second player. The game's trade screen (trade_ui) runs on
// the client with the copies of the second player's character and of the
// other party; what was picked goes back to the host, which moves the items
// as npc_trading::trade() does, with the second player in the avatar's place.
namespace mp::remote_trade
{

// ---- Host ----

// npc_trading::trade() while the second player is talking to np (hook in
// npctrade.cpp). Whether anything was traded.
bool trade_with_npc( npc &guy, npc &np, int cost, const std::string &deal );
// The second player offers the host an exchange; the host agrees or not.
void trade_with_host( npc &guy );

// ---- Client ----

// A "trade" question: the trade screen, and the answer.
void answer( const JsonObject &question );

} // namespace mp::remote_trade

#endif // CATA_SRC_MP_REMOTE_TRADE_H
