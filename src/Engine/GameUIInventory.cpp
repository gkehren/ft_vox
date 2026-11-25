// Legacy E inventory panel (issue #229) and its block-atlas item icons. DERIVED
// presentation only (epic #223 §5): every surface reads the bounded
// sim::InventorySnapshot in GameUIFrame and issues explicit command
// callbacks — no gameplay rule lives here. The drag cursor is UI-local
// presentation (a view of the source slot via InventoryUi.hpp), discarded
// when the panel closes; authoritative moves happen only through commands.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "Engine/GameUI.hpp"

#include <Engine/UiTheme.hpp>
#include <Items/ItemRegistry.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_impl_vulkan.h>
#include <imgui/imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace
{
// One slot: dark backing, inset atlas icon, stack count, selection border.
// `desc` may be null (icon unavailable) — the slot still renders its frame
// and count, so the panel never blocks on texture readiness.
void drawSlot(ImDrawList *dl, const inventoryui::SlotRect &r, const items::ItemStack &stack,
              VkDescriptorSet desc, bool selected, float scale, float rounding)
{
    const ImVec2 min(r.x, r.y), max(r.x + r.w, r.y + r.h);
    dl->AddRectFilled(min, max, IM_COL32(18, 20, 24, 200), rounding);
    if (desc != VK_NULL_HANDLE && !stack.isEmpty())
    {
        const float inset = 3.0f * scale;
        dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(desc)),
                     ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset));
    }
    if (stack.count > 1)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u", unsigned(stack.count));
        const ImVec2 textSize = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(max.x - textSize.x - 2.0f * scale, max.y - textSize.y - 1.0f * scale),
                    IM_COL32(255, 255, 255, 255), buf);
    }
    dl->AddRect(min, max, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(90, 96, 106, 255),
                rounding, 0, selected ? 2.0f * scale : 1.0f * scale);
}

std::string itemLabel(const items::ItemStack &stack)
{
    if (stack.isEmpty())
        return "Empty";
    const std::string_view name = items::itemName(stack.item);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.*s x%u", int(name.size()), name.data(), unsigned(stack.count));
    return buf;
}
} // namespace

// ---------------------------------------------------------------------------
// Item icons: per-atlas-layer ImGui descriptors over the block atlas.
// ---------------------------------------------------------------------------

void GameUI::invalidateItemIcons()
{
    if (m_vk && m_vk->getDevice() != VK_NULL_HANDLE)
    {
        for (size_t i = 0; i < m_iconDescs.size(); ++i)
        {
            if (m_iconDescs[i] != VK_NULL_HANDLE)
                ImGui_ImplVulkan_RemoveTexture(m_iconDescs[i]);
            if (m_iconViews[i] != VK_NULL_HANDLE)
                vkDestroyImageView(m_vk->getDevice(), m_iconViews[i], nullptr);
        }
    }
    m_iconDescs.fill(VK_NULL_HANDLE);
    m_iconViews.fill(VK_NULL_HANDLE);
    m_iconAtlasVersion = 0;
}

VkDescriptorSet GameUI::itemIconDesc(GameUIFrame &frame, TextureType type)
{
    const size_t idx = static_cast<size_t>(type);
    if (idx >= m_iconDescs.size() || !frame.vk || !frame.worldRenderer)
        return VK_NULL_HANDLE;
    // Atlas (re)load: the old layer views reference a destroyed image.
    if (frame.atlasVersion != m_iconAtlasVersion)
    {
        invalidateItemIcons();
        m_iconAtlasVersion = frame.atlasVersion;
    }
    TextureManager &atlas = frame.worldRenderer->getTextureManager();
    if (!atlas.isValid())
        return VK_NULL_HANDLE;
    if (m_iconDescs[idx] != VK_NULL_HANDLE)
        return m_iconDescs[idx];
    if (m_iconViews[idx] == VK_NULL_HANDLE)
    {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = atlas.getImage();
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D; // one array layer as a 2D tile
        vi.format = atlas.getFormat();
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, static_cast<uint32_t>(idx), 1};
        if (vkCreateImageView(frame.vk->getDevice(), &vi, nullptr, &m_iconViews[idx]) != VK_SUCCESS)
            return VK_NULL_HANDLE;
    }
    m_iconDescs[idx] = ImGui_ImplVulkan_AddTexture(atlas.getSampler(), m_iconViews[idx],
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return m_iconDescs[idx];
}

// ---------------------------------------------------------------------------
// Inventory panel (E): 27 main + 9 hotbar grid. Click-to-move/swap and
// right-click-to-split THROUGH COMMAND CALLBACKS ONLY; the drag cursor is
// UI-local presentation, discarded on close (never duplicates items).
// ---------------------------------------------------------------------------

void GameUI::drawInventoryPanel(GameUIFrame &frame)
{
    if (!frame.inventoryPanelOpen || !*frame.inventoryPanelOpen)
    {
        m_drag = {};
        return;
    }
    bool open = *frame.inventoryPanelOpen;
    const float scale = frame.uiScale;
    const float slotSize = ui::scaled(44.0f, scale);
    const float spacing = ui::scaled(4.0f, scale);

    ImGui::SetNextWindowSize(ImVec2(inventoryui::panelWidth(slotSize, spacing) + ui::scaled(24.0f, scale),
                                    inventoryui::panelHeight(slotSize, spacing) + ui::scaled(96.0f, scale)),
                             ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Inventory", &open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextDisabled("Left-click: move/swap · Right-click: split half · Esc/close: cancel drag");
        ImGui::TextDisabled("A half-drop onto a nearly-full slot moves only what fits; the rest stays.");
        ImGui::Separator();
        if (!frame.inventoryInputEnabled)
            m_drag = {};
        ImGui::BeginDisabled(!frame.inventoryInputEnabled);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        for (int slot = 0; slot < inventoryui::kSlotCount; ++slot)
        {
            const inventoryui::SlotRect r =
                inventoryui::panelSlotRect(slot, origin.x, origin.y, slotSize, spacing);
            // Displayed state: the drag source shows its post-drop remainder
            // (presentation only — the sim snapshot is unchanged until a
            // command lands).
            const items::ItemStack shown =
                inventoryui::displayedSlotStack(slot, m_drag, frame.inventory.slots);
            VkDescriptorSet desc = VK_NULL_HANDLE;
            if (!shown.isEmpty())
            {
                const auto block = items::placeableBlockOf(shown.item);
                if (block)
                    desc = itemIconDesc(frame, *block);
            }
            const bool isHotbar = slot < items::kHotbarSize;
            drawSlot(dl, r, shown, desc, isHotbar && slot == frame.inventory.selectedSlot, scale,
                     3.0f * scale);

            ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
            char id[24];
            std::snprintf(id, sizeof(id), "##invslot_%d", slot);
            ImGui::InvisibleButton(id, ImVec2(r.w, r.h));
            const bool left = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            const bool right = ImGui::IsItemClicked(ImGuiMouseButton_Right);
            if (left || right)
            {
                if (m_drag.active())
                {
                    const inventoryui::ClickPlan plan =
                        inventoryui::planDrop(m_drag, slot, right, frame.inventory.slots);
                    switch (plan.command)
                    {
                    case inventoryui::ClickCommand::Move:
                        if (frame.moveStack)
                            frame.moveStack(plan.from, plan.to);
                        break;
                    case inventoryui::ClickCommand::Swap:
                        if (frame.swapStacks)
                            frame.swapStacks(plan.from, plan.to);
                        break;
                    case inventoryui::ClickCommand::Split:
                        if (frame.splitStack)
                            frame.splitStack(plan.from, plan.to);
                        break;
                    case inventoryui::ClickCommand::None:
                        break;
                    }
                    m_drag = {}; // drop (or cancel) ends the drag
                }
                else
                {
                    m_drag = inventoryui::planPickup(slot, right, frame.inventory.slots);
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", itemLabel(frame.inventory.slots[static_cast<size_t>(slot)]).c_str());
        }

        // Cursor stack follows the mouse (foreground: above every window).
        if (m_drag.active())
        {
            const items::ItemStack held = inventoryui::cursorStack(m_drag, frame.inventory.slots);
            VkDescriptorSet desc = VK_NULL_HANDLE;
            if (!held.isEmpty())
            {
                const auto block = items::placeableBlockOf(held.item);
                if (block)
                    desc = itemIconDesc(frame, *block);
            }
            const ImVec2 mouse = ImGui::GetMousePos();
            const inventoryui::SlotRect r{mouse.x - slotSize * 0.5f, mouse.y - slotSize * 0.5f, slotSize,
                                          slotSize};
            drawSlot(ImGui::GetForegroundDrawList(), r, held, desc, false, scale, 3.0f * scale);
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + inventoryui::panelHeight(slotSize, spacing) +
                                                       ui::scaled(6.0f, scale)));
        ImGui::Dummy(ImVec2(inventoryui::panelWidth(slotSize, spacing), ui::scaled(1.0f, scale)));
        ImGui::Separator();
        const items::ItemStack selected = frame.inventory.slots[static_cast<size_t>(frame.inventory.selectedSlot)];
        // Pause gate matches the Q key (Engine): drops are world mutations and
        // stay inert while paused (the sim would freeze the entity anyway).
        const bool dropPaused = frame.paused && *frame.paused;
        ImGui::BeginDisabled(dropPaused);
        if (ImGui::Button("Drop selected stack") && !selected.isEmpty() && frame.dropFromSlot && frame.camera)
            frame.dropFromSlot(frame.inventory.selectedSlot, selected.count, glm::dvec3(frame.camera->getFront()));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Q drops one · 1-9 select");
        ImGui::EndDisabled();
    }
    ImGui::End();
    *frame.inventoryPanelOpen = open;
    if (!open)
        m_drag = {}; // closing mid-drag discards the cursor: no duplication
}
