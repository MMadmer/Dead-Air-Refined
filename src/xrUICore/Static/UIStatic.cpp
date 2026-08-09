#include "pch.hpp"
#include "Windows/UIWindow.h"
#include "UIStatic.h"

#include "XML/UITextureMaster.h"
#include "xrEngine/LightAnimLibrary.h"
#include "Lines/UILines.h"
#include "Include/xrRender/UIRender.h"
#include "Buttons/UIBtnHint.h"
#include "Cursor/UICursor.h"

bool is_in2(const Frect& b1, const Frect& b2);

void lanim_cont::set_defaults()
{
    m_lanim = NULL;
    m_lanim_start_time = -1.0f;
    m_lanim_delay_time = 0.0f;
    m_lanimFlags.zero();
}
void lanim_cont_xf::set_defaults()
{
    lanim_cont::set_defaults();
    m_origSize.set(0, 0);
}

CUIStatic::CUIStatic(pcstr window_name) : CUIWindow(window_name)
{
    m_TextureOffset.set(0.0f, 0.0f);
    m_lanim_xform.set_defaults();
}

CUIStatic::~CUIStatic() { xr_delete(m_pTextControl); }

void CUIStatic::SetXformLightAnim(LPCSTR lanim, bool bCyclic)
{
    if (lanim && lanim[0] != 0)
        m_lanim_xform.m_lanim = LALib.FindItem(lanim);
    else
        m_lanim_xform.m_lanim = NULL;

    m_lanim_xform.m_lanimFlags.zero();

    m_lanim_xform.m_lanimFlags.set(LA_CYCLIC, bCyclic);
    m_lanim_xform.m_origSize = GetWndSize();
}

bool CUIStatic::InitTexture(pcstr texture, bool fatal /*= true*/)
{
    return InitTextureEx(texture, "hud" DELIMITER "default", fatal);
}

void CUIStatic::CreateShader(const char* tex, const char* sh)
{
    m_UIStaticItem.CreateShader(tex, sh);
}

bool CUIStatic::InitTextureEx(pcstr texture, pcstr shader, bool /*fatal = true*/)
{
    LPCSTR res_shname = GEnv.UIRender->UpdateShaderName(texture, shader);
    bool result = CUITextureMaster::InitTexture(texture, &m_UIStaticItem, res_shname);

    Fvector2 p = GetWndPos();
    m_UIStaticItem.SetPos(p.x, p.y);
    return result;
}

void CUIStatic::Draw()
{
    DrawTexture();
    inherited::Draw();
    DrawText();
}

void CUIStatic::DrawText()
{
    if (m_pTextControl)
    {
        if (!fsimilar(m_pTextControl->m_wndSize.x, m_wndSize.x) || !fsimilar(m_pTextControl->m_wndSize.y, m_wndSize.y))
        {
            m_pTextControl->m_wndSize = m_wndSize;
            m_pTextControl->ParseText(true);
        }

        Fvector2 p;
        GetAbsolutePos(p);
        m_pTextControl->Draw(p.x, p.y);
    }
    if (g_statHint->Owner() == this)
        g_statHint->Draw_();
}

#include "Include/xrRender/UIShader.h"

void CUIStatic::DrawTexture()
{
    if (m_bTextureEnable && GetShader() && GetShader()->inited())
    {
        Frect rect;
        GetAbsoluteRect(rect);
        m_UIStaticItem.SetPos(rect.left + m_TextureOffset.x, rect.top + m_TextureOffset.y);

        if (m_bStretchTexture)
        {
            if (Heading())
            {
                if (m_UIStaticItem.GetFixedLTWhileHeading())
                {
                    const float t1 = rect.width();
                    const float t2 = rect.height();
                    rect.y2 = rect.y1 + t1;
                    rect.x2 = rect.x1 + t2;
                }
            }
            m_UIStaticItem.SetSize(Fvector2().set(rect.width(), rect.height()));
        }
        else
        {
            const Frect r = { 0.0f, 0.0f, m_UIStaticItem.GetTextureRect().width(), m_UIStaticItem.GetTextureRect().height() };

            if (Heading())
            {
                const float t1 = rect.width();
                const float t2 = rect.height();
                rect.y2 = rect.y1 + t1;
                rect.x2 = rect.x1 + t2;
            }

            m_UIStaticItem.SetSize(Fvector2().set(r.width(), r.height()));
        }

        if (Heading())
        {
            m_UIStaticItem.Render(GetHeading());
        }
        else if (m_textureRounding > 0.0f)
            DrawRoundedTexture(rect);
        else
            m_UIStaticItem.Render();
    }
}

void CUIStatic::DrawRoundedTexture(const Frect& rect)
{
    Fvector2 ts{};
    GetShader()->GetBaseTextureResolution(ts);
    if (fis_zero(ts.x) || fis_zero(ts.y))
    {
        m_UIStaticItem.Render();
        return;
    }

    Fvector2 lt, rb;
    UI().ClientToScreenScaled(lt, rect.left + m_TextureOffset.x, rect.top + m_TextureOffset.y);
    UI().ClientToScreenScaled(rb, rect.right + m_TextureOffset.x, rect.bottom + m_TextureOffset.y);
    UI().AlignPixel(lt.x);
    UI().AlignPixel(lt.y);
    UI().AlignPixel(rb.x);
    UI().AlignPixel(rb.y);

    const float width = rb.x - lt.x;
    const float height = rb.y - lt.y;
    if (width <= 0.0f || height <= 0.0f)
        return;

    const float radius = _min(m_textureRounding, _min(width, height) * 0.5f);

    const Frect& tex = m_UIStaticItem.GetTextureRect();
    const Fvector2 uv_lt{ tex.x1 / ts.x, tex.y1 / ts.y };
    const Fvector2 uv_rb{ tex.x2 / ts.x, tex.y2 / ts.y };
    const u32 color = m_UIStaticItem.GetTextureColor();

    // Screen position carries straight over to a texture coordinate, so a vertex anywhere on the
    // rounded outline samples exactly what the plain quad would have sampled at that spot.
    const auto push = [&](float x, float y)
    {
        const float u = uv_lt.x + (x - lt.x) / width * (uv_rb.x - uv_lt.x);
        const float v = uv_lt.y + (y - lt.y) / height * (uv_rb.y - uv_lt.y);
        GEnv.UIRender->PushPoint(x, y, 0.0f, color, u, v);
    };

    constexpr u32 corner_segments = 6;
    xr_vector<Fvector2> outline;
    outline.reserve((corner_segments + 1) * 4);

    const Fvector2 centers[4] = { { rb.x - radius, lt.y + radius }, { rb.x - radius, rb.y - radius },
        { lt.x + radius, rb.y - radius }, { lt.x + radius, lt.y + radius } };

    for (u32 corner = 0; corner < 4; ++corner)
    {
        const float base = -PI_DIV_2 + PI_DIV_2 * float(corner);
        for (u32 step = 0; step <= corner_segments; ++step)
        {
            const float angle = base + PI_DIV_2 * float(step) / float(corner_segments);
            outline.emplace_back(Fvector2().set(
                centers[corner].x + radius * _cos(angle), centers[corner].y + radius * _sin(angle)));
        }
    }

    const Fvector2 center{ (lt.x + rb.x) * 0.5f, (lt.y + rb.y) * 0.5f };

    GEnv.UIRender->SetShader(*GetShader());
    GEnv.UIRender->StartPrimitive(outline.size() * 3, IUIRender::ptTriList, UI().m_currentPointType);

    for (size_t i = 0; i < outline.size(); ++i)
    {
        const Fvector2& a = outline[i];
        const Fvector2& b = outline[(i + 1) % outline.size()];

        push(center.x, center.y);
        push(a.x, a.y);
        push(b.x, b.y);
    }

    GEnv.UIRender->FlushPrimitive();
}

void CUIStatic::Update()
{
    inherited::Update();
    // update light animation if defined
    UpdateColorAnimation();

    if (m_lanim_xform.m_lanim)
    {
        if (m_lanim_xform.m_lanim_start_time < 0.0f)
            ResetXformAnimation();

        float t = Device.dwTimeGlobal / 1000.0f;

        if (m_lanim_xform.m_lanimFlags.test(LA_CYCLIC) ||
            (t - m_lanim_xform.m_lanim_start_time) * Device.time_factor() < m_lanim_xform.m_lanim->Length_sec())
        {
            int frame;
            u32 clr = m_lanim_xform.m_lanim->CalculateRGB((t - m_lanim_xform.m_lanim_start_time) / Device.time_factor(), frame);

            EnableHeading_int(true);
            float heading = (PI_MUL_2 / 255.0f) * color_get_A(clr);
            SetHeading(heading);

            float _value = (float)color_get_R(clr);

            float f_scale = _value / 64.0f;
            Fvector2 _sz;
            _sz.set(m_lanim_xform.m_origSize.x * f_scale, m_lanim_xform.m_origSize.y * f_scale);
            SetWndSize(_sz);
        }
        else
        {
            EnableHeading_int(m_bHeading);
            SetWndSize(m_lanim_xform.m_origSize);
        }
    }

    if (CursorOverWindow() && m_stat_hint_text.size() && !g_statHint->Owner() &&
        Device.dwTimeGlobal > m_dwFocusReceiveTime + 700)
    {
        g_statHint->SetHintText(this, m_stat_hint_text.c_str());

        Fvector2 c_pos = GetUICursor().GetCursorPosition();
        Frect vis_rect;
        vis_rect.set(0, 0, UI_BASE_WIDTH, UI_BASE_HEIGHT);

        // select appropriate position
        Frect r;
        r.set(0.0f, 0.0f, g_statHint->GetWidth(), g_statHint->GetHeight());
        r.add(c_pos.x, c_pos.y);

        r.sub(0.0f, r.height());
        if (false == is_in2(vis_rect, r))
            r.sub(r.width(), 0.0f);
        if (false == is_in2(vis_rect, r))
            r.add(0.0f, r.height());

        if (false == is_in2(vis_rect, r))
            r.add(r.width(), 45.0f);

        g_statHint->SetWndPos(r.lt);
    }
}

void CUIStatic::ResetXformAnimation() { m_lanim_xform.m_lanim_start_time = Device.dwTimeGlobal / 1000.0f; }
void CUIStatic::SetShader(const ui_shader& sh) { m_UIStaticItem.SetShader(sh); }

CUILines* CUIStatic::TextItemControl()
{
    if (!m_pTextControl)
    {
        m_pTextControl = xr_new<CUILines>();
        m_pTextControl->SetTextAlignment(CGameFont::alLeft);
    }
    return m_pTextControl;
}

void CUIStatic::AdjustHeightToText()
{
    if (!fsimilar(TextItemControl()->m_wndSize.x, GetWidth()))
    {
        TextItemControl()->m_wndSize.x = GetWidth();
        TextItemControl()->ParseText(true);
    }
    SetHeight(TextItemControl()->GetVisibleHeight());
}

void CUIStatic::AdjustWidthToText()
{
    if (!m_pTextControl)
        return;
    float _len = m_pTextControl->GetFont()->SizeOf_(m_pTextControl->GetText());
    UI().ClientToScreenScaledWidth(_len);
    SetWidth(_len);
}

void CUIStatic::ColorAnimationSetTextureColor(u32 color, bool only_alpha)
{
    SetTextureColor((only_alpha) ? subst_alpha(GetTextureColor(), color) : color);
}

void CUIStatic::ColorAnimationSetTextColor(u32 color, bool only_alpha)
{
    SetTextColor((only_alpha) ? subst_alpha(GetTextColor(), color) : color);
}

void CUIStatic::FillDebugInfo()
{
#ifndef MASTER_GOLD
    CUIWindow::FillDebugInfo();

    if (ImGui::CollapsingHeader(CUIStatic::GetDebugType()))
    {
        ImGui::Checkbox("Enable texture", &m_bTextureEnable);
        ImGui::Checkbox("Stretch texture", &m_bStretchTexture);
        ImGui::DragFloat2("Texture offset", (float*)&m_TextureOffset);
        //m_UIStaticItem->FillDebugInfo(); // XXX: to do
        ImGui::Checkbox("Enable heading", &m_bHeading);
        ImGui::Checkbox("Const heading", &m_bConstHeading);
        ImGui::DragFloat("Heading", &m_fHeading);
        //m_pTextControl->FillDebugInfo(); // XXX: to do
        ImGui::LabelText("Stat hint text", "%s", m_stat_hint_text.empty() ? "" : m_stat_hint_text.c_str());
    }
#endif
}

void CUIStatic::OnFocusLost()
{
    inherited::OnFocusLost();

    if (g_statHint->Owner() == this)
        g_statHint->Discard();
}
