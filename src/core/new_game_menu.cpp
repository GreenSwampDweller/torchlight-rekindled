#include "core/new_game_menu.h"

namespace tl::core {

// builds these fifteen `CEGUI::String`s in this order.
const char* const kNewGameGrid[kNewGameRows][kNewGameColumns] = {
    {"Destroyer", "Alchemist", "Vanquisher"},
    {"EditBox", "EditBox", "EditBox"},
    {"Dog", "Cat", "Chakawary"},
    {"EditBoxPet", "EditBoxPet", "EditBoxPet"},
    {"DoneButton", "DoneButton", "DoneButton"},
};

// DIVERGENCE: the top row goes on with the mods' classes, as the PC's creation screen lists
// the classes a mod adds; every other row repeats its last window in those columns.
NewGameGrid new_game_grid(const std::vector<std::string>& mod_classes) {
    NewGameGrid grid(kNewGameRows);
    for (int row = 0; row < kNewGameRows; ++row) {
        for (int column = 0; column < kNewGameColumns; ++column) {
            grid[static_cast<size_t>(row)].emplace_back(kNewGameGrid[row][column]);
        }
        for (const std::string& name : mod_classes) {
            grid[static_cast<size_t>(row)].push_back(
                row == 0 ? name : kNewGameGrid[row][kNewGameColumns - 1]);
        }
    }
    return grid;
}

// Each axis wraps to the far edge rather than by a modulus:
// below zero is the last column (row), and past the last is zero.
NewGameFocus step_new_game_focus(const NewGameGrid& grid, NewGameFocus from, int dx, int dy,
                                 const std::function<bool(const std::string&)>& visible) {
    const int rows = static_cast<int>(grid.size());
    const int columns = rows == 0 ? 0 : static_cast<int>(grid[0].size());
    if (columns == 0) return from;
    NewGameFocus at = from;
    // DIVERGENCE: the original loops until it meets a visible window and would
    // spin forever on a grid with none (or on a (0, 0) step from a hidden
    // one). One try per cell visits every cell a step can reach.
    for (int tries = 0; tries < rows * columns; ++tries) {
        int column = at.column + dx;
        column = column < 0 ? columns - 1 : (column < columns ? column : 0);
        int row = at.row + dy;
        row = row < 0 ? rows - 1 : (row < rows ? row : 0);
        at.column = column;
        at.row = row;
        if (visible(new_game_window(grid, at))) return at;
    }
    return from;
}

NewGameFocus step_new_game_focus(NewGameFocus from, int dx, int dy,
                                 const std::function<bool(const std::string&)>& visible) {
    static const NewGameGrid kGrid = new_game_grid({});
    return step_new_game_focus(kGrid, from, dx, dy, visible);
}

// PORT: the window of the New Game screen's grid cell (the console's layout).
const std::string& new_game_window(const NewGameGrid& grid, NewGameFocus focus) {
    static const std::string kNone;
    if (focus.row < 0 || static_cast<size_t>(focus.row) >= grid.size() || focus.column < 0 ||
        static_cast<size_t>(focus.column) >= grid[static_cast<size_t>(focus.row)].size()) {
        return kNone;
    }
    return grid[static_cast<size_t>(focus.row)][static_cast<size_t>(focus.column)];
}

const char* new_game_window(NewGameFocus focus) {
    if (focus.row < 0 || focus.row >= kNewGameRows || focus.column < 0 ||
        focus.column >= kNewGameColumns) {
        return "";
    }
    return kNewGameGrid[focus.row][focus.column];
}

// PORT: what an `onClick` of the New Game layout asks for.
NewGameCommand new_game_command(const std::string& on_click) {
    if (on_click == "GUINEWGAME") return NewGameCommand::Done;
    if (on_click == "GUISELECT1") return NewGameCommand::ChooseClass;
    if (on_click == "GUISELECT2") return NewGameCommand::ChoosePet;
    if (on_click == "GUISELECT49") return NewGameCommand::EditPlayerName;
    if (on_click == "GUISELECT50") return NewGameCommand::EditPetName;
    return NewGameCommand::None;
}

std::string new_game_keyboard_name(const std::string& typed) {
    // `basic_string(+0x138)`, then `append(13 - size, 0)` below 13
    // or `erase(13, npos)` from 13.
    constexpr size_t kChars = 13;
    size_t chars = 0;
    size_t i = 0;
    while (i < typed.size() && chars < kChars) {
        if (typed[i] == '\0') break;  // the window's text ends at a NUL
        const unsigned char lead = static_cast<unsigned char>(typed[i]);
        // The console's characters are UTF-16 units: one past the BMP is two.
        size_t width = 1;
        size_t units = 1;
        if (lead >= 0xf0) {
            width = 4;
            units = 2;
        } else if (lead >= 0xe0) {
            width = 3;
        } else if (lead >= 0xc0) {
            width = 2;
        }
        // DIVERGENCE: `erase(13)` can cut such a pair in half; the port's text
        // is UTF-8 and cannot hold half a character, so the pair goes whole.
        if (i + width > typed.size() || chars + units > kChars) break;
        i += width;
        chars += units;
    }
    return typed.substr(0, i);
}

// `CNewGameMenu::handle_SubmitPet`: the names
// handed over only when both windows hold something.
NewGameSubmission new_game_submit_pet(const std::string& player_name, const std::string& pet_name) {
    NewGameSubmission out;
    // `+0x114`'s text (the pet's box) and `+0x110`'s (the player's) both not
    // empty; the answer is 1 either way.
    if (pet_name.empty() || player_name.empty()) return out;
    out.submitted = true;
    out.player_name = player_name;
    out.pet_name = pet_name;
    return out;
}

// DoneButton's visibility is `!(player empty || pet empty)`.
bool new_game_done_visible(const std::string& player_name, const std::string& pet_name) {
    return !player_name.empty() && !pet_name.empty();
}

}  // namespace tl::core
