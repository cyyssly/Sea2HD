// SPDX-FileCopyrightText: 2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_SEA2_HD_H
#define DOSBOX_SEA2_HD_H

#include <cstdint>
#include <string>

// Kept as a cheap segment gate in the normal CPU core. Zero disables capture.
extern uint16_t sea2_hd_trace_loadseg;

// One-off reconnaissance switch: aggregate string-op addresses while tracing.
extern bool sea2_hd_string_trace;

// Countdown window (in instructions) right after a font glyph read.
extern uint32_t sea2_hd_glyph_watch;

void SEA2_HD_TraceGlyphSite(uint16_t cs, uint16_t ip);

void SEA2_HD_TraceStringOp(uint16_t cs, uint16_t ip, uint16_t ds, uint16_t si,
                           uint16_t es, uint16_t di, uint16_t cx, uint8_t opcode);
void SEA2_HD_TraceBlit(uint16_t ds, uint16_t src, uint16_t x, uint16_t y,
                       uint16_t width, uint16_t height, uint16_t planes);
void SEA2_HD_ScanFont();
void SEA2_HD_TraceDosOpen(const char* name, uint16_t handle, uint16_t cs,
                          uint16_t ip, uint16_t name_ds, uint16_t name_dx);
void SEA2_HD_TraceDosRead(uint16_t ds, uint16_t dx, uint16_t len, uint16_t handle,
                          uint32_t file_offset, uint16_t cs, uint16_t ip);
void SEA2_HD_TraceInt21Ring(uint16_t ss, uint16_t sp, const uint32_t* history,
                            unsigned count);
void SEA2_HD_TraceBlitterEntry(uint16_t ip);
void SEA2_HD_NotifyGlyphRecord(uint16_t record1);
void SEA2_HD_TraceInt16(uint16_t cs, uint16_t ip, uint8_t ah, uint16_t ax);
void SEA2_HD_TraceDosCall(uint16_t cs, uint16_t ip, uint8_t ah);
void SEA2_HD_ProfileIp(uint16_t cs, uint16_t ip);
void SEA2_HD_TraceEntry(uint16_t cs, uint16_t ip, uint16_t ss, uint16_t sp,
                        const uint16_t* words, unsigned count);
void SEA2_HD_DumpStringTrace();

// 1.PAT glyph record -> Unicode code point (0 means not in the table)
uint32_t SEA2_HD_GlyphTraditional(uint16_t record);
uint32_t SEA2_HD_GlyphSimplified(uint16_t record);
void SEA2_HD_VerifyGlyphMap();

void SEA2_HD_SetMode(const std::string& mode, const std::string& assets_path);
// 场景门控：打开后，稳态只运行当前场景需要的高清管线；换场景后的
// 识别期仍然全部运行。关闭时行为与未加门控完全一致。
void SEA2_HD_SetSceneGate(bool enabled);
// 启动加速：MAIN.EXE 运行期间第一次切到 640x480 16 色模式（密码界面）时，
// 标记进入密码界面，由 INT 16h 钩子返回一次回车，直接进入主菜单。
void SEA2_HD_NotifyVideoMode(uint8_t mode);
void SEA2_HD_SetAutoEnter(bool enabled);
// INT 16h 钩子：密码界面期间用一次 Enter 直接应答键盘读取。
bool SEA2_HD_TrySkipPasswordKey(uint16_t& key);
bool SEA2_HD_IsActive();
bool SEA2_HD_NeedsFrame();
void SEA2_HD_AdvanceFrame();
void SEA2_HD_NotifyProgramExecuted(uint16_t pspseg, uint16_t loadseg,
                                   const char* canonical_name);
void SEA2_HD_NotifyProgramTerminated(uint16_t pspseg);
void SEA2_HD_ClearProgram();
void SEA2_HD_TraceKaoEntry(uint16_t ip);

void SEA2_HD_Composite(const uint32_t* source, int source_width, int source_height,
                       uint32_t* destination, int destination_pitch);

// 640x480 16 色（mode 12h）显存写入跟踪。VGA_UnchainedEGA_Handler 每写一个
// planar 字节调用一次，用来识别不经过 blitter 的大块窗口填充。关闭 HD 时该
// 开关为 false，页处理器里只剩一次布尔判断。
extern bool sea2_hd_vga_write_trace;
void SEA2_HD_NoteVgaWrite(uint32_t planar_offset, uint8_t plane_mask,
                          uint16_t cs, uint16_t ip);

// Reconnaissance switch: while tracing, save the 640x480 frame buffer as a PNG
// every N frames (0 turns it off) so a screen can be looked at next to the
// trace log.
void SEA2_HD_SetFrameDump(int interval);
void SEA2_HD_DumpFrame(const void* pixels, int pitch, int width, int height);

#endif
