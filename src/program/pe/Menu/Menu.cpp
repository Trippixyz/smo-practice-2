#include "pe/Menu/Menu.h"
#include "Player/PlayerActorHakoniwa.h"
#include "al/Library/Controller/JoyPadUtil.h"
#include "al/Library/LiveActor/ActorFlagFunction.h"
#include "al/Library/LiveActor/ActorPoseKeeper.h"
#include "al/Library/LiveActor/LiveActor.h"
#include "al/Library/Nerve/NerveUtil.h"
#include "al/Library/Scene/Scene.h"
#include "al/Library/Thread/FunctorV0M.h"
#include "imgui.h"
#include "pe/Menu/Action.h"
#include "pe/Menu/BoolMenuComponent.h"
#include "pe/Menu/ButtonMenuComponent.h"
#include "pe/Menu/DummyMenuComponent.h"
#include "pe/Menu/EnumMenuComponent.h"
#include "pe/Menu/IntMenuComponent.h"
#include "pe/Menu/MenuComponent.h"
#include "pe/Menu/QuickActionMenu.h"
#include "pe/Menu/UserConfig.h"
#include "pe/Util/Localization.h"
#include "pe/Util/Nerve.h"
#include "pe/Util/Offsets.h"
#include "pe/Util/Type.h"
#include "rs/Util/PlayerUtil.h"
#include "util/modules.hpp"
#include <algorithm>

namespace pe {

sead::Heap*& getMenuHeap()
{
    static sead::Heap* sHeap { nullptr };
    return sHeap;
}

SEAD_SINGLETON_DISPOSER_IMPL(Menu)

bool someBOol = false;

Menu::Menu()
{
    getConfig() = new UserConfig;
    pe::loadConfig();

    mCategories[0].name = "Thing";
    mCategories[0].components.allocBuffer(2, nullptr);
    mCategories[0].components.pushBack(new BoolMenuComponent(&someBOol, "yeah"));
    // mCategories[0].components.pushBack(new EnumMenuComponent<int>(&value, ball, "Random int", false, 35));

    mCategories[1].name = "keybinds";
    mCategories[1].components.allocBuffer(4, nullptr);
    mCategories[1].components.pushBack(new EnumMenuComponent<int>(reinterpret_cast<int*>(&getConfig()->mDUpBind), sActionNames, "dpadup", true));
    mCategories[1].components.pushBack(new EnumMenuComponent<int>(reinterpret_cast<int*>(&getConfig()->mDDownBind), sActionNames, "dpaddown", true));
    mCategories[1].components.pushBack(new EnumMenuComponent<int>(reinterpret_cast<int*>(&getConfig()->mDLeftBind), sActionNames, "dpadleft", true));
    mCategories[1].components.pushBack(new EnumMenuComponent<int>(reinterpret_cast<int*>(&getConfig()->mDRightBind), sActionNames, "dpadright", true));

    mCategories[2].name = "settings";
    mCategories[2].components.allocBuffer(1, nullptr);
    static constexpr const char* languageNames[] {
        "English", "日本語", "Deutsch"
    };
    mCategories[2].components.pushBack(new EnumMenuComponent<int>(reinterpret_cast<int*>(&getConfig()->currentLanguage), languageNames, "Language", false));

    mComponents.allocBuffer(4, nullptr);
    mComponents.pushBack(new QuickActionMenu(*this));
}

void Menu::update(al::Scene* scene)
{
    mScene = scene;

    if (mTimer % 3600 == 0) {
        pe::saveConfig();
    }
    mTimer++;

    
    for (int i = 0; i < mComponents.size(); i++)
    {
        IComponent* component = mComponents[i];
        if (component)
            component->update();
    }
}

void Menu::draw()
{
    updateInput();

    for (int i = 0; i < mComponents.size(); i++)
    {
        IComponent* component = mComponents[i];
        if (component)
            component->draw();
    }

    if (!mIsEnabled) {
        if (al::isPadTriggerUp(-1))
            callAction(getConfig()->mDUpBind);
        if (al::isPadTriggerDown(-1))
            callAction(getConfig()->mDDownBind);
        if (al::isPadTriggerLeft(-1))
            callAction(getConfig()->mDLeftBind);
        if (al::isPadTriggerRight(-1))
            callAction(getConfig()->mDRightBind);

        return;
    }

    mBgSize = ImVec2(0, 0);
    for (int i = 0; i < IM_ARRAYSIZE(mCategories); i++) {
        ImVec2 textSize = ImGui::GetIO().Fonts->Fonts[0]->CalcTextSizeA(sCategoryFontSize, FLT_MAX, 0.f, pe::getLocalizedString(mCategories[i].name));

        int width = sCategorySpacing + textSize.x + sCategorySpacing;
        mBgSize.x = std::max(mBgSize.x, (float)width);
    }
    mBgSize.y += getCategoryHeight(IM_ARRAYSIZE(mCategories));

    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), mBgSize, IM_COL32(0, 0, 0, 128));
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, getCategoryHeight(mCurrentCategory)), ImVec2(mBgSize.x, getCategoryHeight(mCurrentCategory + 1)), IM_COL32(255, 255, 255, 76));

    ImVec2 size = ImVec2(0, 0);
    for (int i = 0; i < IM_ARRAYSIZE(mCategories); i++) {
        ImVec2 textSize = ImGui::GetIO().Fonts->Fonts[0]->CalcTextSizeA(sCategoryFontSize, FLT_MAX, 0.f, pe::getLocalizedString(mCategories[i].name));
        size.y += sCategorySpacing;

        ImGui::GetForegroundDrawList()->AddText(ImGui::GetIO().Fonts->Fonts[0], sCategoryFontSize, ImVec2(size.x + sCategorySpacing, size.y), IM_COL32(255, 255, 255, 255), pe::getLocalizedString(mCategories[i].name));

        size.y += textSize.y;
        size.y += sCategorySpacing;
    }

    if (mIsExpandedCurrentCategory)
        drawExpandedCategory();
}

void Menu::drawExpandedCategory()
{
    auto& cat = mCategories[mCurrentCategory];
    int subX = 0, subY = 0;
    int y = getCategoryHeight(mCurrentCategory);

    for (MenuComponent& component : cat.components) {
        ImVec2 size = component.getSize();
        subX = std::max((float)subX, size.x);
        subY += size.y;
    }

    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(mBgSize.x, y), ImVec2(mBgSize.x + subX, y + subY), IM_COL32(0, 0, 0, 128));

    getCurrentCategory().components[mCurrentComponentInCategory]->setIsHovered(true);

    subY = 0;
    {
        for (int i = 0; i < cat.components.size(); i++) {
            MenuComponent* component = cat.components[i];
            if (component == nullptr)
                continue;

            ImVec2 size = component->getSize();
            ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(mBgSize.x, y + subY), ImVec2(mBgSize.x + subX, y + subY + size.y), IM_COL32(0, 0, 0, 128));
            if (mCurrentComponentInCategory == i) {
                ImU32 color = mIsFocusedOnCurrentComponent ? IM_COL32(255, 60, 60, 76) : IM_COL32(255, 255, 255, 76);
                ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(mBgSize.x, y + subY), ImVec2(mBgSize.x + subX, y + subY + size.y), color);
            }
            component->draw(ImVec2(mBgSize.x, y + subY));
            component->setIsHovered(false);

            subY += size.y;
        }
    }
}

void Menu::updateInput()
{
    if (al::isPadHoldL(-1) && al::isPadTriggerLeft(-1)) {
        mIsEnabled = !mIsEnabled;
        return;
    }

    if (!mIsExpandedCurrentCategory) {
        if (al::isPadTriggerDown(-1)) {
            mCurrentCategory++;
        } else if (al::isPadTriggerUp(-1)) {
            mCurrentCategory--;
        }

        if (al::isPadTriggerRight(-1)) {
            mIsExpandedCurrentCategory = true;
        }
    } else {
        if (!mIsFocusedOnCurrentComponent) {
            if (al::isPadTriggerDown(-1)) {
                mCurrentComponentInCategory++;
            } else if (al::isPadTriggerUp(-1)) {
                mCurrentComponentInCategory--;
            } else if (al::isPadTriggerLeft(-1)) {
                mIsExpandedCurrentCategory = false;
            }
        }

        if (mCurrentComponentInCategory < 0)
            mCurrentComponentInCategory = 0;
        if (mCurrentComponentInCategory >= getCurrentCategory().components.size())
            mCurrentComponentInCategory = getCurrentCategory().components.size() - 1;

        if (getCurrentCategory().components[mCurrentComponentInCategory]->canHaveFocus()) {
            if (al::isPadTriggerPressLeftStick(-1))
                mIsFocusedOnCurrentComponent = !mIsFocusedOnCurrentComponent;
            getCurrentCategory().components[mCurrentComponentInCategory]->setIsFocused(mIsFocusedOnCurrentComponent);
        } else
            mIsFocusedOnCurrentComponent = false;
    }

    if (mCurrentCategory >= IM_ARRAYSIZE(mCategories))
        mCurrentCategory = IM_ARRAYSIZE(mCategories) - 1;
    if (mCurrentCategory < 0)
        mCurrentCategory = 0;
}

void Menu::callAction(ActionType type)
{
    if (mScene && mScene->mIsAlive && mScene->getNerveKeeper()->getCurrentNerve() == util::getNerveAt(offsets::StageSceneNrvPlay)) {
        PlayerActorBase* playerBase = reinterpret_cast<PlayerActorBase*>(rs::getPlayerActor(mScene));
        switch (type) {
        case ActionType::KillScene: {
            mScene->kill();
            return;
        }
        case ActionType::SavePosition: {
            if (playerBase) {
                if (PlayerActorHakoniwa* player = util::typeCast<PlayerActorHakoniwa>(playerBase)) {
                    al::LiveActor* hack = player->getCurrentHack();
                    al::LiveActor* move = hack != nullptr ? hack : player;

                    mSavedPosition = al::getTrans(move);
                    mSavedQuat = move->mPoseKeeper->getQuat();
                }
            }

            return;
        }
        case ActionType::LoadPosition: {
            if (playerBase) {
                if (PlayerActorHakoniwa* player = util::typeCast<PlayerActorHakoniwa>(playerBase)) {
                    al::LiveActor* hack = player->getCurrentHack();
                    al::LiveActor* move = hack != nullptr ? hack : player;

                    if (hack == nullptr)
                        player->startDemoPuppetable();

                    al::offCollide(move);
                    al::setTrans(move, mSavedPosition);
                    al::updatePoseQuat(move, mSavedQuat);
                    al::onCollide(move);
                    if (hack == nullptr)
                        player->endDemoPuppetable();
                }
            }
            return;
        }
        default:
            break;
        }
    } else {
        switch (type) {
        default:
            break;
        }
    }
}

} // namespace pe