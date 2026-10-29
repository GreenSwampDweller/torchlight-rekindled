// The console's character creation screen: what the pad does on it (`CNewGameMenu`, Xbox
// only). A grid of window names walked with wrap-around, skipping what is hidden; A fires
// the focused window's `onClick`, B leaves.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tl::core {

inline constexpr int kNewGameColumns = 3;
inline constexpr int kNewGameRows = 5;

/// The focus grid, row by row.
extern const char* const kNewGameGrid[kNewGameRows][kNewGameColumns];

/// Where the focus is. `CNewGameMenu +0x20C` (column) and `+0x210` (row); both
/// start at zero, which is the Destroyer.
struct NewGameFocus {
    int column = 0;
    int row = 0;
};

/// Step the focus by (dx, dy) and keep stepping until it lands on a window
/// that is visible, wrapping at both edges. `visible` answers
/// for a window name the way `CEGUI::Window::isVisible(false)` does.
///
/// A step of (0, 0) re-tests where the focus already is, which is how the
/// screen places it when it opens (`CNewGameMenu::vfunc_7`).
NewGameFocus step_new_game_focus(NewGameFocus from, int dx, int dy,
                                 const std::function<bool(const std::string&)>& visible);

/// The window the focus is on.
const char* new_game_window(NewGameFocus focus);

/// A focus grid, row by row: `kNewGameGrid`, its top row going on with the mods' classes.
using NewGameGrid = std::vector<std::vector<std::string>>;
NewGameGrid new_game_grid(const std::vector<std::string>& mod_classes);
NewGameFocus step_new_game_focus(const NewGameGrid& grid, NewGameFocus from, int dx, int dy,
                                 const std::function<bool(const std::string&)>& visible);
const std::string& new_game_window(const NewGameGrid& grid, NewGameFocus focus);

/// What pressing A on a window does, from the `onClick` its layout authors.
/// The layout spells these `GUISELECTn` and `GUINEWGAME`; `CNewGameMenu`'s
/// handler receives them as command numbers 0x0D + n and 2.
enum class NewGameCommand {
    None,
    ChooseClass,     ///< `GUISELECT1`, 0x0E: the window's name is the class
    ChoosePet,       ///< `GUISELECT2`, 0x0F: the window's name is the pet
    EditPlayerName,  ///< `GUISELECT49`, 0x3E: the keyboard, for the player
    EditPetName,     ///< `GUISELECT50`, 0x3F: the keyboard, for the pet
    Done,            ///< `GUINEWGAME`, 2: start, if Done is showing
};

NewGameCommand new_game_command(const std::string& on_click);

/// Whether Done is shown: only while **both** names have at least one
/// character. Hidden, it is also skipped by the focus, so the
/// bottom row cannot be reached until both names are filled in.
bool new_game_done_visible(const std::string& player_name, const std::string& pet_name);

/// `CNewGameMenu::update`, the half that takes the
/// keyboard's answer: the typed name cut to 13 characters (`erase(13)`). Characters, not
/// bytes: `typed` is UTF-8.
std::string new_game_keyboard_name(const std::string& typed);

/// What `CNewGameMenu::handle_SubmitPet` hands the game
/// after the pet's name comes back: the two names, only when both hold something.
struct NewGameSubmission {
    bool submitted = false;
    std::string player_name;
    std::string pet_name;
};
NewGameSubmission new_game_submit_pet(const std::string& player_name, const std::string& pet_name);

/// The two names' defaults, `L"Player"` and `L"Spot"`
/// (set by the screen's builder).
inline constexpr const char* kDefaultPlayerName = "Player";
inline constexpr const char* kDefaultPetName = "Spot";

/// The class and the pet the screen opens on, `L"Destroyer"` and `L"Dog"`
/// (`CNewGameMenu::vfunc_7`).
inline constexpr const char* kDefaultClass = "Destroyer";
inline constexpr const char* kDefaultPet = "Dog";

/// The longest a name may be. `XamShowKeyboardUI` is handed a 13-character
/// buffer, and one of those is the terminator.
inline constexpr unsigned kNewGameNameChars = 12;

/// What the console's keyboard says over each name.
///
inline constexpr const char* kPlayerNameTitle = "Player Name";
inline constexpr const char* kPlayerNameGuide = "Enter a name for your player!";
inline constexpr const char* kPetNameTitle = "Pet Name";
inline constexpr const char* kPetNameGuide = "Enter a name for your pet!";

}  // namespace tl::core
