/* The guest has no kernel headers (miniBox's sysroot is musl alone), and no
 * kernel either: Boost.Asio and Boost.Process ask this file which Linux they
 * are built for, to choose epoll, eventfd and the like. The answer is "none
 * of them": version 0. */
#pragma once
#define LINUX_VERSION_CODE 0
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
