// HDR-capable presentation: OpenGL keeps rendering everything, into an image that is
// shared with Vulkan (EXT_memory_object / EXT_semaphore); Vulkan only copies that image
// to its swapchain, which can be SDR or HDR (scRGB or HDR10). OpenGL on its own can't
// output HDR on most drivers. Vulkan is loaded at runtime through SDL.
#pragma once

#include "gl.h"

#include <memory>
#include <string>

struct SDL_Window;

class VkPresenter {
public:
    // What the swapchain currently expects the shared image to contain.
    enum class Output {
        Sdr,    // sRGB-encoded values 0..1 (like a normal window)
        ScRgb,  // linear BT.709, 1.0 = 80 nits, may exceed 1
        Hdr10,  // BT.2020 primaries, SMPTE ST 2084 (PQ) encoded
    };

    VkPresenter();
    ~VkPresenter();

    // `window` must have been created with SDL_WINDOW_VULKAN; an OpenGL context must be
    // current (on any window). `allowHdr` = false always uses an SDR swapchain.
    bool init(SDL_Window *window, bool allowHdr, std::string &error);
    void shutdown();

    // Call before rendering a frame of the given size. Returns the GL framebuffer to render
    // the final, encoded image into (0 on failure: skip the frame).
    GLuint beginFrame(int width, int height);
    // Hands the frame to Vulkan and presents it.
    void endFrame();

    // The window's HDR state may have changed (SDL_EVENT_WINDOW_HDR_STATE_CHANGED, moved
    // to another display, ...): rebuild the swapchain on the next frame.
    void refresh();
    void setAllowHdr(bool allow);
    // Extensive logging: a step-by-step frame every ~2 s, swapchain details on every rebuild.
    void setTrace(bool on);
    // HDR setting "On": use HDR swapchain formats when offered, even if SDL reports SDR.
    void setPreferHdr(bool prefer);
    // false: don't wait for the display (immediate / mailbox present mode), for stress tests.
    void setVsync(bool on);
    // How much brighter than SDR white the clipped highlights (the glow) may get in HDR.
    void setBoost(float b) { boost_ = b; }

    Output output() const;
    // From SDL: the SDR white level in scRGB units, and the HDR headroom above it.
    float sdrWhite() const;
    float headroom() const;
    const char *describe() const; // short text for the menu, e.g. "HDR10" / "SDR"
    // Whether the window's surface offers an HDR swapchain format (scRGB or HDR10), i.e. HDR
    // output would actually happen. Valid after init().
    bool offersHdr() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    float boost_ = 3.0f;
};
