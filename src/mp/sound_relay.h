#pragma once
#ifndef CATA_SRC_MP_SOUND_RELAY_H
#define CATA_SRC_MP_SOUND_RELAY_H

#include <optional>
#include <string>

class JsonObject;

// Sends the sound effects the host's game plays to the second player, whose
// game plays them too. Hooks sit in sdlsound.cpp and sounds.cpp. Sounds are
// placed relative to the host's character, so they are right while the two
// characters stay together.
namespace mp::sound_relay
{

// Host side: the arguments of sfx::play_variant_sound() and friends.
void variant( const std::string &id, const std::string &variant, const std::string &season,
              const std::optional<bool> &is_indoors, const std::optional<bool> &is_night,
              int volume, std::optional<double> angle_degrees, double pitch_min, double pitch_max );
void ambient( const std::string &id, const std::string &variant, const std::string &season,
              const std::optional<bool> &is_indoors, const std::optional<bool> &is_night,
              int volume, int channel, int fade_in_duration, double pitch, int loops );
void fade_channel( int channel, int duration );

// Client side: plays a "sfx" message.
void play( const JsonObject &message );

} // namespace mp::sound_relay

#endif // CATA_SRC_MP_SOUND_RELAY_H
