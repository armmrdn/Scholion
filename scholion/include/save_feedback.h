#pragma once
#include <chrono>

enum class SaveFeedbackType { None, Manual, Auto };

extern SaveFeedbackType                          g_save_feedback_type;
extern std::chrono::steady_clock::time_point     g_save_feedback_time;
