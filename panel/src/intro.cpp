// "What's new" introductions (see intro.h).
#include "intro.h"

#include "panel.h"

#include <algorithm>

namespace intro {

namespace {

/** Every intro, oldest first. Ids are never reused. */
const std::vector<Intro> kIntros = {
    {"eyeball_gaze", PanelTab::EyeFit, -1, "0.11.0",
     "新しく: 眼球の視線（試験）",
     "目のカメラに映る瞳の形から目の向きを出す、新しい「視線のもと」だよ。\n"
     "開発者のテストでは Valve の視線より正確で、ヘッドセットがずれても狂いにくかったよ。\n"
     "使うには「視線のもと」を［眼球（試験）］にして、目合わせを最初からしてね（四隅の点も出て約 32 秒。"
     "［真ん中だけ合わせ直す］ではできないよ）。\n"
     "目のカメラが要るよ。いつでも［Valve］に戻せるよ。",
     "New: Eyeball gaze (test)",
     "A new gaze source: the eye cameras work out where each eye points from the shape of its pupil.\n"
     "In the developer's tests it was more accurate than Valve's gaze, and kept up when the headset shifted.\n"
     "To try it, set \"Gaze source\" to Eyeball (test), then do a whole eye fit (about 32 s, with four corner dots; "
     "\"Re-center only\" doesn't make it).\n"
     "It needs the eye cameras. You can switch back to Valve any time.",
     Row::GazeSource},
};

}  // namespace

const std::vector<Intro>& all() {
    return kIntros;
}

std::vector<std::string> allIds() {
    std::vector<std::string> ids;
    for (const Intro& intro : kIntros) ids.emplace_back(intro.id);
    return ids;
}

const Intro* find(const std::string& id) {
    for (const Intro& intro : kIntros) {
        if (id == intro.id) return &intro;
    }
    return nullptr;
}

const Intro* due(PanelTab tab, int page, const std::vector<std::string>& seen) {
    for (const Intro& intro : kIntros) {
        if (intro.tab != tab || (intro.page >= 0 && intro.page != page)) continue;
        if (std::find(seen.begin(), seen.end(), intro.id) == seen.end()) return &intro;
    }
    return nullptr;
}

bool freshInstall(bool panelStateExists, bool configExists, bool hasRecords) {
    return !panelStateExists && !configExists && !hasRecords;
}

const char* title(const Intro& intro, Language language) {
    return language == Language::Ja ? intro.titleJa : intro.titleEn;
}

const char* body(const Intro& intro, Language language) {
    return language == Language::Ja ? intro.bodyJa : intro.bodyEn;
}

}  // namespace intro
