#pragma once

#include "std/vector.h"
#include "std/string.h"

#include "Debug.hpp"
#include "Message.hpp"
#include "Settings.hpp"
#include "../net/utenyaa_game.h"   /* g_Game for online-mode gating */
#include "../net/utenyaa_net.h"    /* unet_get_data() for online char_id lookup */
extern "C" {
#include "../cc_download.h"        /* cc_download_get_sprite_id for custom HUD portrait */
}

namespace UI
{
    namespace Messages
    {
        struct NavigatePrevious : MessageType<NavigatePrevious> {};
        struct NavigateNext : MessageType<NavigateNext> {};
        struct SelectPrevious : MessageType<SelectPrevious> {};
        struct SelectNext : MessageType<SelectNext> {};
        struct PerformAction : MessageType<PerformAction> {};
        struct Draw : MessageType<Draw> {};
        struct GetText : MessageType<GetText> { std::string text; };

        struct UpdatePlayer : MessageType<UpdatePlayer>
        {
            size_t index;
            int32_t health;
            size_t powerupType;
            int32_t cooldownFrames;   /* shoot cooldown frames remaining; 0 = ready */
        };

        struct UpdateTime : MessageType<UpdateTime>
        {
            size_t currentTime;
            UpdateTime(const size_t& time) : currentTime(time) {}
        };
    }

    struct GUIElement : public IMessageHandler
    {
        virtual void HandleMessages(const Message& message) override {}
    };

    struct Button : public GUIElement
    {
        size_t x = 0;
        size_t y = 0;
        Button(const size_t& posX, const size_t& posY) :x(posX), y(posY) {}

        virtual void HandleMessages(const Message& message) override {}
    };

    struct ActionButton : public Button
    {
        virtual void PerformAction() = 0;
        virtual std::string Text() const = 0;

        void HandleMessages(const Message& message) override
        {
            if (message.IsType<Messages::PerformAction>()) { return PerformAction(); }
            if (auto* getText = message.TryCast<Messages::GetText>()) { getText->text = std::move(Text()); }
        }

        ActionButton(const size_t& posX, const size_t& posY) : Button(posX, posY) {}
    };

    struct Selector : public Button
    {
        virtual void SelectNext() = 0;
        virtual void SelectPrevious() = 0;
        virtual std::string Text() const = 0;

        void HandleMessages(const Message& message) override
        {
            if (message.IsType<Messages::SelectNext>()) { return SelectNext(); }
            if (message.IsType<Messages::SelectPrevious>()) { return SelectPrevious(); }
            if (auto* getText = message.TryCast<Messages::GetText>()) { getText->text = std::move(Text()); }
        }

        Selector(const size_t& posX, const size_t& posY) : Button(posX, posY) {}
    };

    template <typename T>
    concept DerivedFromButton = std::is_base_of<Button, T>::value;

    template <DerivedFromButton... Buttons>
    class ButtonGroup : public GUIElement
    {
    private:
        static constexpr size_t buttonCount = sizeof... (Buttons);
        size_t current = 0;
        Button* buttons[buttonCount];

    public:
        ButtonGroup() : buttons{ new Buttons()... } {}

        ~ButtonGroup() { for (Button* toggle : buttons) { delete toggle; } }

        void NavigatePrevious() { current = (current == 0) ? buttonCount - 1 : current - 1; }

        void NavigateNext() { current = (current + 1) % buttonCount; }

        void Draw()
        {
            Messages::GetText getTextMessage;
            for (size_t i = 0; i < buttonCount; i++)
            {
                buttons[i]->HandleMessages(getTextMessage);
                jo_printf(buttons[i]->x - 2, buttons[i]->y, "%s%s", i == current ? "->" : "  ", getTextMessage.text.c_str());
            }
        }

        void HandleMessages(const Message& message) override
        {
            if (message.IsType<Messages::NavigateNext>()) { return NavigateNext(); }
            if (message.IsType<Messages::NavigatePrevious>()) { return NavigatePrevious(); }
            if (message.IsType<Messages::Draw>()) { return Draw(); }
            buttons[current]->HandleMessages(message);
        }
    };

    struct HUD : public GUIElement
    {
        inline static int headLocations[4][2] = { { 10, 3 }, { 270, 3 }, { 3, 48 }, { 278, 48 } };

        /* Cooldown text cells per player. Coordinates are 8-pixel
         * character cells (40 wide × 28 tall on 320×224 PAL).
         *
         *   P0 head occupies col 1-5 row 0-4, hearts col 6-9 row 0-2,
         *      powerup col 6-8 row 2-3. Free below at row 5.
         *   P1 mirror on right: head col 33-37, hearts col 30-32, powerup
         *      col 32. Free below at row 5.
         *   P2/P3: same layout shifted down to row 6-10. Free at row 11.
         *
         * 3 chars wide ("RDY" / "%02d ") so it overwrites in place. */
        inline static int cooldownCell[4][2] = { { 1, 5 }, { 33, 5 }, { 1, 11 }, { 33, 11 } };

        int32_t playerHealth[4] = { 6, 6, 6, 6 };
        size_t powerupType[4] = { 0, 0, 0, 0 };
        int32_t cooldownFrames[4] = { -1, -1, -1, -1 };  /* -1 = remote/unknown (no render), 0 = RDY, >0 = cooling */

        void HandleMessages(const Message& message)
        {
            if (auto* playerUpdate = message.TryCast<Messages::UpdatePlayer>())
            {
                playerHealth[playerUpdate->index]   = playerUpdate->health;
                powerupType[playerUpdate->index]    = playerUpdate->powerupType;
                cooldownFrames[playerUpdate->index] = playerUpdate->cooldownFrames;
            }

            if (auto* timeUpdate = message.TryCast<Messages::UpdateTime>())
            {
                /* In online mode the canonical match timer is rendered
                 * by OnlineBridge::DrawGameplayOverlay (centered at row 1,
                 * server-authoritative). Skip the offline HUD position
                 * (cell 18,0) so we don't have two writers fighting
                 * mid-frame for adjacent cells, which the user reported
                 * as a strobing/overlap on the in-game timer. */
                if (!g_Game.isOnlineMode)
                {
                    jo_printf(18, 0, "%01d:%02d", timeUpdate->currentTime / 60, timeUpdate->currentTime % 60);
                }
            }
            Draw();
        }

        void Draw()
        {

			for (size_t player = 0; player < Settings::PlayerCount; player++)
			{
				int offset = 3;

				if (playerHealth[player] <= 0)
				{
					offset = 0;
				}
				else if (playerHealth[player] < 3)
				{
					offset = 1;
				}
				else if (playerHealth[player] < 5)
				{
					offset = 2;
				}

				if (player == 0 || player == 2)
				{
					for(int heart = 0; heart < JO_MIN(playerHealth[player], 3); heart++)
					{
						jo_sprite_draw3D2(2, headLocations[player][X] + 38 + (heart * 8), headLocations[player][Y], 50);
					}

					for(int heart = 3; heart < playerHealth[player]; heart++)
					{
						jo_sprite_draw3D2(2, headLocations[player][X] + 42 + ((heart - 3) * 8), headLocations[player][Y] + 9, 50);
					}
				}
				else if (player == 1 || player == 3)
				{
					for(int heart = 0; heart < JO_MIN(playerHealth[player], 3); heart++)
					{
						jo_sprite_draw3D2(2, headLocations[player][X] - 4 - (heart * 8), headLocations[player][Y], 50);
					}

					for(int heart = 3; heart < playerHealth[player]; heart++)
					{
						jo_sprite_draw3D2(2, headLocations[player][X] - 8 - ((heart - 3) * 8), headLocations[player][Y] + 9, 50);
					}
				}

				if (powerupType[player] > 0 && (player == 0 || player == 2))
				{
					jo_sprite_draw3D2(powerupType[player] - 1, headLocations[player][X] + 40, headLocations[player][Y] + 16, 50);
				}
				else if (powerupType[player] > 0 && (player == 1 || player == 3))
				{
					jo_sprite_draw3D2(powerupType[player] - 1, headLocations[player][X] - 8, headLocations[player][Y] + 16, 50);
				}

				/* Big-circle character portrait. HUD.PAK ships 4 builtin
				 * portraits × 4 health states, indexed by player slot
				 * (sprite_id = 3 + player*4 + offset). When the player
				 * has a CUSTOM character (char_id >= 5), we'd otherwise
				 * show the built-in portrait for slot N — operator-
				 * reported "the big circle preview of the character in
				 * game when playing as custom character should be
				 * replaced with an enlarged version of their forward
				 * facing sprite."
				 *
				 * Approach: look up the player's character_id from the
				 * net state (online) or fall back to slot index
				 * (offline). If char_id is a registered custom
				 * (cc_download_get_sprite_id returns >= 0), draw the
				 * custom's frame-0 sprite scaled 2x at the same head
				 * location. Otherwise the built-in HUD portrait
				 * renders unchanged. */
				int hud_char_id = (int)player;   /* default — offline / unknown */
				if (g_Game.isOnlineMode)
				{
					const unet_state_data_t* nd = unet_get_data();
					for (int ri = 0; ri < nd->game_roster_count; ri++)
					{
						if (nd->game_roster[ri].active &&
							nd->game_roster[ri].id == (uint8_t)player)
						{
							hud_char_id = (int)nd->game_roster[ri].character_id;
							break;
						}
					}
				}
				int custom_sprite = -1;
				if (hud_char_id >= 5)
				{
					custom_sprite = cc_download_get_sprite_id(hud_char_id - 5);
				}
				if (custom_sprite >= 0)
				{
					/* Scale the 16x16 custom sprite up to roughly the
					 * same footprint as the built-in HUD portrait
					 * (~32x32). Scale 2.0× is the cleanest doubling on
					 * VDP1's bilinear-free scaler. Using
					 * jo_sprite_change_sprite_scale + draw + restore
					 * scopes the zoom to this one sprite — the next
					 * jo_sprite_draw3D2 calls (cooldown text, etc.)
					 * use the unscaled default. */
					jo_sprite_change_sprite_scale(2.0f);
					jo_sprite_draw3D2(custom_sprite,
						headLocations[player][X],
						headLocations[player][Y], 50);
					jo_sprite_restore_sprite_scale();
				}
				else
				{
					jo_sprite_draw3D2(3 + (player * 4) + offset,
						headLocations[player][X], headLocations[player][Y], 50);
				}

				/* Cooldown indicator. -1 means we don't have data for this
				 * slot (remote online players — we don't see their reload
				 * timer). Render fixed-width 4 chars so the prior frame's
				 * digits/RDY text gets fully overwritten in place — no
				 * jo_clear_screen needed and no leftover artifacts. */
				if (cooldownFrames[player] >= 0)
				{
					int col = cooldownCell[player][0];
					int row = cooldownCell[player][1];
					if (cooldownFrames[player] > 0)
						jo_printf(col, row, "%02d ", cooldownFrames[player]);
					else
						jo_printf(col, row, "RDY");
				}
			}
        }

    };

    static inline HUD HudHandler;
};
