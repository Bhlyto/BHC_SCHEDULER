/* mongoose_config.h — user configuration overrides for Mongoose.
 *
 * MG_ARCH is defined at compile time via CMakeLists.txt:
 *   -DMG_ARCH=2  (MG_ARCH_WIN32) on Windows
 *   -DMG_ARCH=1  (MG_ARCH_UNIX)  on Linux/macOS
 *
 * This file is intentionally minimal: all platform detection is handled
 * by CMake so this include is never reached under normal builds.
 * It exists only as a fallback to satisfy the #include directive in mongoose.h
 * when the compiler cannot determine the architecture automatically.
 */
