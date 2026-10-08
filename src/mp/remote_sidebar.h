#pragma once
#ifndef CATA_SRC_MP_REMOTE_SIDEBAR_H
#define CATA_SRC_MP_REMOTE_SIDEBAR_H

class JsonOut;
class npc;

// The second player's sidebar: the host puts it together for the remote NPC
// with the same texts and colors as its own sidebar (display::*), as lines
// with color tags, and the client just prints them.
namespace mp::remote_sidebar
{

// Writes the members of a "sidebar" message (without "type").
void write( JsonOut &json, const npc &guy );

} // namespace mp::remote_sidebar

#endif // CATA_SRC_MP_REMOTE_SIDEBAR_H
