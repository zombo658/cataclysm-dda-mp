#pragma once
#ifndef CATA_SRC_MP_REMOTE_PROMPT_H
#define CATA_SRC_MP_REMOTE_PROMPT_H

#include <optional>
#include <string>
#include <vector>

#include "color.h"
#include "coordinates.h"

class JsonObject;
class uilist;

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

// ---- Client ----

// Shows the question and sends the answer.
void answer( const JsonObject &question );

} // namespace mp::remote_prompt

#endif // CATA_SRC_MP_REMOTE_PROMPT_H
