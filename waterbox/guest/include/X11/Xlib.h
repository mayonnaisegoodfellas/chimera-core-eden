/* The guest has no window system. Eden's renderer always carries its Vulkan
 * backend, whose headers name Xlib's and Wayland's types on Linux; these are
 * the names and nothing behind them (the backend is never used: the core
 * draws with OpenGL through the GPU bridge). */
#pragma once
typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef XID Window;
typedef unsigned long VisualID;
