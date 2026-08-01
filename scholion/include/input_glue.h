#pragma once
// GLFW input callbacks — the mouse/cursor/scroll/key/focus/drop handlers that translate raw window
// events into canvas/selection/tool/panel actions. This is the app's input integration layer; it
// reaches into most subsystems through their headers. Registered from main() via glfwSet*Callback.
// (framebuffer_size_callback stays in main.cpp — it touches GL directly.)
// Implementation in src/input_glue.cpp.
struct GLFWwindow;

void glfw_error_callback(int error, const char* description);
void mouse_button_callback(GLFWwindow* w, int button, int action, int mods);
void cursor_pos_callback(GLFWwindow* w, double x, double y);
void scroll_callback(GLFWwindow* w, double xoff, double yoff);
void key_callback(GLFWwindow* w, int key, int scancode, int action, int mods);
void focus_callback(GLFWwindow* w, int focused);
void drop_callback(GLFWwindow* w, int count, const char** paths);

extern bool g_clip_valid;   // text-box clipboard occupancy; new_project clears it across projects
