#pragma once

class CUIWindow;
class CUIStatic;

class XRUICORE_API CUICursor : public pureRender, public CDeviceResetNotifier, public CUIResetNotifier
{
    Fvector2 vPos{};
    CUIStatic* m_static{};
    Fvector2 vPrevPos{};
    Fvector2 correction;
    bool bVisible{};
    bool m_bound_to_system_cursor{};
    // 3D PDA: frame stamp of the last rasterization into the PDA screen texture. While it
    // matches the current frame, the ordinary fullscreen OnRender (cursor AND button hints)
    // is suppressed - they already live on the device screen.
    u32 m_rt_frame = u32(-1);
    bool m_in_rt_pass = false;

    void InitInternal();

public:
    void RenderToPdaScreen();
    CUICursor();
    ~CUICursor() override;

    void Show() { bVisible = true; }
    void Hide() { bVisible = false; }

    [[nodiscard]]
    bool IsVisible() const { return bVisible; }

    void OnRender() override;
    void OnDeviceReset() override;
    void OnUIReset() override;

    void WarpToWindow(const CUIWindow* wnd, bool center = false);
    void UpdateCursorPosition(Fvector2 pos);

    void SetUICursorPosition(Fvector2 pos);

    [[nodiscard]]
    Fvector2 GetCursorPosition() const;

    [[nodiscard]]
    Fvector2 GetCursorPositionDelta() const;
};
