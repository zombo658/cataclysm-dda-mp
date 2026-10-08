#pragma once
#ifndef CATA_SRC_MP_CLIENT_UI_H
#define CATA_SRC_MP_CLIENT_UI_H

// The second player's side inside the game: connect to the host and play
// the remote-controlled character. Needs no world loaded.
namespace mp::client_ui
{

// Asks for the host's address, connects and runs until the player leaves
// or the connection drops.
void run_join_screen();

} // namespace mp::client_ui

#endif // CATA_SRC_MP_CLIENT_UI_H
