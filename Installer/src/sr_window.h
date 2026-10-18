#pragma once
#include <cstdint>
#include <string>

struct SrWindow;

SrWindow* sr_window_open(const char* title, int width, int height);
void sr_window_close(SrWindow* w);

float sr_window_scale(SrWindow* w);

bool sr_window_pump(SrWindow* w, int timeout_ms);

void sr_window_size(SrWindow* w, int* width, int* height);

void sr_window_present(SrWindow* w, const uint32_t* pixels, int width, int height);

std::string sr_window_take_drop(SrWindow* w);
