#include "core/skill_grid.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>

namespace tl::core {

// PORT: the skills tab's cells for a PC class file. DIVERGENCE (data): the console's
// class files place every skill on two columns; a PC file's third column has no
// cell there, so its skills are re-packed rather than left off the tab.
size_t pack_two_column_skill_grid(std::vector<ClassSkill>& skills, bool* repacked) {
    if (repacked != nullptr) *repacked = false;
    const bool three_columns = std::any_of(skills.begin(), skills.end(), [](const ClassSkill& s) {
        return s.column >= kSkillColumns;
    });
    if (!three_columns) return 0;
    if (repacked != nullptr) *repacked = true;

    size_t unplaced = 0;
    for (int pane = 0; pane < kSkillPanes; ++pane) {
        // The pane's skills by row, each row in `COLUMN` order (file order on a tie).
        std::array<std::vector<size_t>, kSkillRows> rows;
        for (size_t i = 0; i < skills.size(); ++i) {
            const ClassSkill& s = skills[i];
            if (s.pane == pane && s.row >= 0 && s.row < kSkillRows) {
                rows[static_cast<size_t>(s.row)].push_back(i);
            }
        }
        for (std::vector<size_t>& row : rows) {
            std::stable_sort(row.begin(), row.end(), [&](size_t a, size_t b) {
                return skills[a].column < skills[b].column;
            });
        }

        std::array<std::array<bool, kSkillColumns>, kSkillRows> taken{};
        std::vector<size_t> carried;
        for (int r = 0; r < kSkillRows; ++r) {
            const std::vector<size_t>& own = rows[static_cast<size_t>(r)];
            // What waited from the row above goes first; the row's own leftmost
            // skills wait for the next row when there is no room for them.
            std::vector<size_t> placed = carried;
            std::vector<size_t> waiting;
            if (placed.size() > static_cast<size_t>(kSkillColumns)) {
                waiting.assign(std::next(placed.begin(), kSkillColumns), placed.end());
                placed.resize(kSkillColumns);
            }
            const size_t room = kSkillColumns - placed.size();
            const auto first_kept = std::next(
                own.begin(), static_cast<std::ptrdiff_t>(own.size() - std::min(room, own.size())));
            // `waiting` holds copies, no iterator of `placed`.
            // cppcheck-suppress invalidContainer
            waiting.insert(waiting.end(), own.begin(), first_kept);
            placed.insert(placed.end(), first_kept, own.end());
            // cppcheck-suppress invalidContainer
            carried = std::move(waiting);
            for (size_t c = 0; c < placed.size(); ++c) {
                ClassSkill& s = skills[placed[c]];
                s.row = r;
                s.column = static_cast<int32_t>(c);
                taken[static_cast<size_t>(r)][c] = true;
            }
        }

        // Past the last row: the lowest free cell left in the pane.
        for (size_t i : carried) {
            ClassSkill& s = skills[i];
            s.row = -1;
            s.column = -1;
            for (int r = kSkillRows - 1; r >= 0 && s.row < 0; --r) {
                for (int c = 0; c < kSkillColumns; ++c) {
                    bool& cell = taken[static_cast<size_t>(r)][static_cast<size_t>(c)];
                    if (cell) continue;
                    cell = true;
                    s.row = r;
                    s.column = c;
                    break;
                }
            }
            if (s.row < 0) ++unplaced;
        }
    }
    return unplaced;
}

}  // namespace tl::core
