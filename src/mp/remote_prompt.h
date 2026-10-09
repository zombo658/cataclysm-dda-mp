#pragma once
#ifndef CATA_SRC_MP_REMOTE_PROMPT_H
#define CATA_SRC_MP_REMOTE_PROMPT_H

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "color.h"
#include "coordinates.h"

class JsonObject;
class JsonOut;
class uilist;
struct talk_data;

// Questions the game asks while the host does something for the second
// player (menus of furniture, "Really...?", directions, numbers) go to the
// second player instead of the host: the host sends the question, waits for
// the answer and goes on with it. Hooks in uilist::query(),
// query_popup::query(), choose_direction() and
// string_input_popup::query_string().
namespace mp::remote_prompt
{

// ---- Host ----

// While alive, questions go to the client.
class asking_client
{
    public:
        asking_client();
        ~asking_client();
        asking_client( const asking_client & ) = delete;
        asking_client &operator=( const asking_client & ) = delete;
};
bool active();
// While alive, questions are the host's own again (to ask the host about
// something the second player does).
class host_question
{
    public:
        host_question();
        ~host_question();
        host_question( const host_question & ) = delete;
        host_question &operator=( const host_question & ) = delete;
    private:
        int saved_depth;
};
// A question of another module: sends it, waits and returns the answer's
// line (std::nullopt: no answer). Asked only while active().
std::optional<std::string> ask_json( const std::string &kind,
                                     const std::function<void( JsonOut & )> &write );
// Lines from the client that came while waiting for an answer.
std::vector<std::string> take_deferred();

// The hooks; std::nullopt when the question is the host's own.
std::optional<int> ask_uilist( const uilist &menu );
// The chosen action (as query_popup::result::action).
std::optional<std::string> ask_popup( const std::string &text, const std::vector<std::string> &actions,
                                      const std::string &category, bool allow_cancel, bool allow_anykey );
// The outer optional: whether asked; the inner one: the direction or cancel.
std::optional<std::optional<tripoint_rel_ms>> ask_direction( const std::string &message,
        bool allow_vertical );
// The outer optional: whether asked; the inner one: the text or cancel.
std::optional<std::optional<std::string>> ask_string( const std::string &title,
        const std::string &description, const std::string &text, int width, bool only_digits );

// dialogue::opt(): the NPC's line and the responses; the chosen response
// (negative: leave), or std::nullopt when not the second player's talk.
std::optional<int> ask_dialogue( const std::string &npc_name, const std::string &line,
                                 const std::string &speaker, const nc_color &speaker_color,
                                 const std::vector<talk_data> &responses, const std::vector<bool> &selectable );
// A new conversation starts: the client opens a fresh dialogue window.
void new_conversation();
// game::peek( p ) (peeking through curtains, ...): the second player peeks
// on their own screen. False when it's the host's peek.
bool peek( const tripoint_bub_ms &p );

// ---- Client ----

// Shows the question and sends the answer.
void answer( const JsonObject &question );
// The answer to a question of id.
void send_answer( int id, const std::function<void( JsonOut & )> &write );

} // namespace mp::remote_prompt

#endif // CATA_SRC_MP_REMOTE_PROMPT_H
