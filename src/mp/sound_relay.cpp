#include "mp/sound_relay.h"

#include <optional>
#include <sstream>
#include <string>

#include "json.h"
#include "mp/net.h"
#include "sounds.h"
#include "units.h"

namespace mp::sound_relay
{

namespace
{

// Nothing to do on the client, or on a host nobody is connected to.
bool relaying()
{
    return net::has_client();
}

void write_conditions( JsonOut &json, const std::string &id, const std::string &variant,
                       const std::string &season, const std::optional<bool> &is_indoors,
                       const std::optional<bool> &is_night, const int volume )
{
    json.member( "type", "sfx" );
    json.member( "id", id );
    json.member( "variant", variant );
    json.member( "season", season );
    if( is_indoors ) {
        json.member( "indoors", *is_indoors );
    }
    if( is_night ) {
        json.member( "night", *is_night );
    }
    json.member( "volume", volume );
}


} // namespace

void variant( const std::string &id, const std::string &variant, const std::string &season,
              const std::optional<bool> &is_indoors, const std::optional<bool> &is_night,
              const int volume, const std::optional<double> angle_degrees, const double pitch_min,
              const double pitch_max )
{
    if( !relaying() ) {
        return;
    }
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    write_conditions( json, id, variant, season, is_indoors, is_night, volume );
    json.member( "kind", "variant" );
    if( angle_degrees ) {
        json.member( "angle", *angle_degrees );
        json.member( "pitch_min", pitch_min );
        json.member( "pitch_max", pitch_max );
    }
    json.end_object();
    net::send_line( os.str() );
}

void ambient( const std::string &id, const std::string &variant, const std::string &season,
              const std::optional<bool> &is_indoors, const std::optional<bool> &is_night,
              const int volume, const int channel, const int fade_in_duration, const double pitch,
              const int loops )
{
    if( !relaying() ) {
        return;
    }
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    write_conditions( json, id, variant, season, is_indoors, is_night, volume );
    json.member( "kind", "ambient" );
    json.member( "channel", channel );
    json.member( "fade_in", fade_in_duration );
    json.member( "pitch", pitch );
    json.member( "loops", loops );
    json.end_object();
    net::send_line( os.str() );
}

void fade_channel( const int channel, const int duration )
{
    if( !relaying() ) {
        return;
    }
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "type", "sfx" );
    json.member( "kind", "fade" );
    json.member( "channel", channel );
    json.member( "duration", duration );
    json.end_object();
    net::send_line( os.str() );
}

#if defined(SDL_SOUND)
static std::optional<bool> optional_bool( const JsonObject &message, const std::string &name )
{
    if( !message.has_member( name ) ) {
        return std::nullopt;
    }
    return message.get_bool( name );
}

void play( const JsonObject &message )
{
    message.allow_omitted_members();
    const std::string kind = message.get_string( "kind", "variant" );
    if( kind == "fade" ) {
        sfx::fade_audio_channel( static_cast<sfx::channel>( message.get_int( "channel" ) ),
                                 message.get_int( "duration", 0 ) );
        return;
    }
    const std::string id = message.get_string( "id", "" );
    const std::string variant = message.get_string( "variant", "default" );
    const std::string season = message.get_string( "season", "" );
    const std::optional<bool> indoors = optional_bool( message, "indoors" );
    const std::optional<bool> night = optional_bool( message, "night" );
    const int volume = message.get_int( "volume", 100 );
    if( kind == "ambient" ) {
        sfx::play_ambient_variant_sound( id, variant, season, indoors, night, volume,
                                         static_cast<sfx::channel>( message.get_int( "channel" ) ),
                                         message.get_int( "fade_in", 0 ), message.get_float( "pitch", -1.0 ),
                                         message.get_int( "loops", -1 ) );
    } else if( message.has_member( "angle" ) ) {
        sfx::play_variant_sound( id, variant, season, indoors, night, volume,
                                 units::from_degrees( message.get_float( "angle" ) ),
                                 message.get_float( "pitch_min", -1.0 ), message.get_float( "pitch_max", -1.0 ) );
    } else {
        sfx::play_variant_sound( id, variant, season, indoors, night, volume );
    }
}
#else
// A game built without sound has nothing to play them with.
void play( const JsonObject & ) {}
#endif

} // namespace mp::sound_relay
