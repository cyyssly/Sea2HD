// SPDX-FileCopyrightText: 2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sea2_hd.h"
#include "sea2_glyph_map.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cpu/registers.h"
#include "dosbox.h"
#include "hardware/input/mouse.h"
#include "hardware/memory.h"
#include "utils/bgrx8888.h"
#include "utils/string_utils.h"

#include <SDL_image.h>

namespace {

constexpr uint16_t Kao64Entry = 0x7cdd;
constexpr uint16_t Kao48Entry = 0x7d4c;
constexpr size_t SignatureSize = 32;
constexpr int MaxTraceEvents = 256;

constexpr int HdScale = 4;
constexpr int VgaWidth = 640;
constexpr int VgaHeight = 480;
constexpr int ContentYOffset = 40;
constexpr int Kao64Width = 64;
constexpr int Kao64Height = 80;
constexpr int Kao64EntryCount = 256;
constexpr size_t MaxDrawCommands = 32;

// A box is compared as "is this pixel equal to its right/below neighbour".
// The same artwork always produces the same pattern, while a different
// picture, a cleared box or a repainted background breaks it at once. This is
// what makes the test safe to run on every frame.
constexpr int KaoRightPairs    = (Kao64Width - 1) * Kao64Height;
constexpr int KaoDownPairs     = Kao64Width * (Kao64Height - 1);
constexpr int KaoRelationBits  = KaoRightPairs + KaoDownPairs;
constexpr int KaoRelationBytes = (KaoRelationBits + 7) / 8;

// A box that still holds plain background has almost no structure, and it must
// never become the reference.
constexpr int KaoMinRelationStructure = (KaoRelationBits * 1) / 100;

// A blink or a small portrait animation moves a couple of dozen pixels. New
// artwork, a cleared box or a different scene move far more than this.
constexpr int KaoRelationTolerance = (KaoRelationBits * 10) / 100;

// The reference can only be taken once the box holds the very same picture a
// few frames in a row, so a half drawn portrait, the pointer or a dialog that
// is still appearing is never frozen.
constexpr uint8_t KaoLockStableFrames = 3;

// A portrait that the game draws into a window is complete in the single blit
// that creates the box, so waiting for its pattern to repeat only leaves the low
// resolution face on screen for a moment. A box that plainly holds a finished
// portrait is therefore drawn right away; the reference is still taken by the
// locking rule, from a picture that really repeats.
constexpr int KaoDialogPreviewMinStructure = (KaoRelationBits * 10) / 100;

// The game fades its own screen in by writing progressively brighter pixels
// into a true-colour framebuffer, so a box can be locked half way through a
// fade. Dimming collapses neighbouring colours into equality, so the finished
// picture is always the *densest* pattern: a fade only ever adds detail, while a
// cleared box, a covered box or a menu redraw is sparser or unrelated. Only a
// denser pattern may replace the reference; that keeps the portrait on screen
// during a fade without ever painting it over other artwork.
constexpr uint8_t KaoMaxReferenceUpdates = 250;

// A window, a text label, the mouse pointer or a redrawn background puts pixels
// inside the box that are not the portrait, while a fade only rescales them.
// The box is therefore also compared pixel by pixel, after the brightness has
// been normalised away. Only those pixels keep the game's own artwork, so a
// pointer passing over a face no longer switches the whole box back to the low
// resolution picture.
constexpr int KaoForeignPixelTolerance = 36;

// The brightness is fitted on the pixels that already agree with the fit. A
// window or a dialog inside the box is not part of that fit, so it can no
// longer drag the gain and the offset away from the portrait and turn a large
// part of an intact face into false "foreign" pixels.
constexpr int KaoFitInlierTolerance = KaoForeignPixelTolerance * 3;
constexpr int KaoMinFitInlierRatio  = 20;

// Only a box that holds nothing but the picture may refresh the stored pixels.
constexpr int KaoMaxForeignPixels = 40;

// Below this many matching pixels the box no longer shows the portrait at all,
// and drawing there would leave a piece of artwork behind on another screen.
// A dialog, a label or a window is drawn as a block, so the pixels that do not
// belong to the picture form long runs inside a row. Such a block may cover a
// large part of the box and the rest still shows the portrait; a different
// picture scatters its mismatches all over the box instead.
constexpr int KaoOverlayRunRatio = 70;

constexpr int KaoMinOverlayMatchingPixels = (Kao64Width * Kao64Height * 25) / 100;
constexpr int KaoMinCleanMatchingPixels   = (Kao64Width * Kao64Height * 90) / 100;

// A face that is not covered at all still differs in a handful of pixels after
// the brightness was fitted away, a fade even more, so a few foreign pixels are
// always tolerated.
constexpr int KaoMinFittedMatchingPixels = (Kao64Width * Kao64Height * 70) / 100;

// A window, a dialog and its "yes/no" buttons are flat panels far wider than a
// portrait box, so their paper continues past the left or the right edge of the
// box, while a colour of the portrait itself never leaves it. The paper can be
// almost the same colour as the picture underneath, and then the pixel test
// above cannot tell it apart and the artwork is painted over the panel exactly
// where that happens. Every stretch of one colour that leaves the box like that
// is therefore handed back to the game as a whole.
constexpr int KaoPanelProbe = 32;
constexpr int KaoPanelReach = 16;

// The "yes/no" confirmation box is small enough to stay inside a portrait box,
// so the test above never sees it. It is a panel all the same: long stretches of
// one single colour, painted over a picture that has texture in those very
// pixels. A flat stretch of the portrait itself always carries the same colour
// in the stored pixels, so the two can be told apart without knowing where the
// game put the box - which follows the pointer, but never by a distance that is
// known in advance.
constexpr int KaoFlatPanelRun      = 8;
constexpr int KaoFlatPanelMinArea  = 64;

// A stored pixel reference can be taken while the screen is still fading, and
// then it never matches again. Only a box whose pattern is still practically
// the stored one may take a new reference: a dialog, a label or another scene
// changes the pattern far more than this.
constexpr int KaoStaleReferenceTolerance = (KaoRelationBits * 2) / 100;
constexpr uint8_t KaoMaxRelocks          = 8;

// A box that never repeats itself cannot be verified frame by frame. After
// this many frames without a stable picture it is drawn unconditionally again,
// which is exactly how it behaved before per-frame verification existed. The
// delay is long on purpose: an unconditional draw over other artwork is worse
// than leaving the box untouched.
constexpr uint16_t KaoSettleGiveUpFrames = 600;

// The segment word of the far call at bytes 20-21 is relocated at load time.
constexpr std::array<uint8_t, SignatureSize> Kao64Signature = {
        0x55, 0x8b, 0xec, 0x81, 0xec, 0x00, 0x01, 0x53,
        0x52, 0x50, 0xa1, 0xda, 0x03, 0x8d, 0x9e, 0x00,
        0xff, 0x9a, 0x0e, 0x75, 0x00, 0x00, 0xb8, 0x80,
        0x07, 0x50, 0xff, 0xb6, 0xfe, 0xfe, 0x8d, 0x86,
};

constexpr std::array<uint8_t, SignatureSize> Kao48Signature = {
        0x55, 0x8b, 0xec, 0x81, 0xec, 0x00, 0x01, 0x53,
        0x52, 0x50, 0xa1, 0xda, 0x03, 0x8d, 0x9e, 0x00,
        0xff, 0x9a, 0x0e, 0x75, 0x00, 0x00, 0x2b, 0xc0,
        0x50, 0x8b, 0x8e, 0xfe, 0xfe, 0x81, 0xc1, 0x80,
};

enum class AssetState { Unloaded, Loaded, Missing };

struct Asset {
	AssetState state = AssetState::Unloaded;
	std::vector<uint8_t> rgba = {};
};

struct DrawCommand {
	uint16_t entry = 0;
	uint16_t x = 0;
	uint16_t y = 0;
	// The only thing remembered about the box is the equality pattern the game
	// left there. It is rewritten only when the same picture is found brightened
	// by an unfinished fade, never when the box shows something else.
	bool locked         = false;
	bool reported       = false;
	bool matched_last   = true;
	bool masked_last    = false;
	uint8_t stable      = 0;
	uint8_t updates     = 0;
	uint16_t unsettled  = 0;
	std::array<uint8_t, KaoRelationBytes> reference = {};
	// The very pixels of the box as they were when the portrait was last seen
	// untouched, used to notice a window, a label or the pointer drawn on top.
	bool pixels_valid = false;
	std::array<uint32_t, Kao64Width * Kao64Height> pixels = {};
	// One flag per source pixel: set when the box holds something else at that
	// spot. Only the flagged 4x4 blocks keep the game's own artwork.
	std::array<uint8_t, Kao64Width * Kao64Height> covered = {};
	// Set when the foreign pixels of this frame form a solid block (a dialog, a
	// label, a window) instead of being scattered over the picture (a different
	// portrait or another scene). A block may hide a large part of the box and
	// the rest still shows the portrait, so less of it has to match.
	bool overlay_like = false;
	// How the stored picture maps onto the box this frame: current = gain *
	// stored + offset, per channel (B, G, R). A screen fade is exactly such a
	// transform, so the artwork is dimmed with the very same numbers instead of
	// staying lit while the rest of the screen goes dark.
	std::array<float, 3> fade_gain   = {1.0f, 1.0f, 1.0f};
	std::array<float, 3> fade_offset = {0.0f, 0.0f, 0.0f};
	// A reference that was taken while the screen was still fading can become
	// unusable. Taking a new one is allowed, but only a few times: a box that
	// keeps failing must not repaint itself back and forth.
	uint8_t relocks = 0;
};

// The character selection screen is the only screen that shows several
// portraits at once. The game always draws them at these six places, and while
// the screen is up the picture inside a box cannot become another picture, so
// these boxes are followed by a rule of their own instead of the generic
// per-frame verification below.
//
// The screen is recognised by its content, not by the commands: the commands
// stay in the list for the rest of the session, so counting them would keep the
// screen "active" long after it is gone and paint portraits over the next one.
// A screen fade only rescales the brightness, which leaves the equality pattern
// of a portrait almost untouched, so a box that still shows its picture differs
// in a couple of pairs while another screen - flat map water, a harbour, a menu
// - changes far more of them. The box is compared against the frozen pattern
// with a generous allowance, because a dialog or the pointer may slice through
// a picture without replacing the screen.
constexpr int KaoSceneIntactTolerance = (KaoRelationBits * 50) / 100;
constexpr int KaoSceneMinIntactBoxes  = 4;

// The six places hold a portrait on the character selection screen and nothing
// else in the whole game, so the commands of that screen are removed once it is
// gone instead of being followed for the rest of the session. Two independent
// signs say it is gone: the game paints every screen change from or to black,
// and the six portraits together are dense with edges, so a map, a harbour or a
// menu leaves the boxes far flatter than the frozen picture.
constexpr int KaoBlackScreenLuma       = 10;
constexpr int KaoLitScreenLuma         = 20;
constexpr int KaoSceneMinDetailedBoxes = 2;
constexpr int KaoSceneRetireFrames     = 90;

constexpr std::array<std::pair<uint16_t, uint16_t>, 6> ScenePortraitBoxes = {
        {
                {512, 168},
                {328, 256},
                {280, 80},
                {184, 56},
                {104, 280},
                {56, 184},
        },
};

bool is_scene_portrait_box(const DrawCommand& command)
{
	for (const auto& box : ScenePortraitBoxes) {
		if (command.x == box.first && command.y == box.second) {
			return true;
		}
	}
	return false;
}

// Text glyphs are 16x14 cells drawn by the same planar blitter. HD replaces
// them with 4x simplified glyphs from a baked atlas.
constexpr int TextGlyphWidth  = 16;
constexpr int TextGlyphHeight = 14;
constexpr int TextGlyphBytes  = 28;
constexpr uint16_t StagingGlyphOffset = 0xf812;
constexpr size_t MaxTextCells = 1200;

// A cell the pointer does not hide and that still cannot be read has lost the
// glyph it was registered for: the screen it belonged to is gone. Such a record
// must not stay drawable, or the pointer would paint the characters of the
// previous screen back on top of the one that replaced it.
constexpr uint8_t TextCellMissLimit = 3;

using GlyphMask = std::array<uint8_t, TextGlyphBytes>;

// The text state is where the game drew a glyph plus the glyph itself. Every
// frame still identifies the cell again from the framebuffer, which is what
// follows a highlight colour change or a menu re-draw. The remembered glyph is
// only used when the pointer sits on the cell and its own pixels are no longer
// enough to name the character: the blit that drew it still names it.
struct TextCell {
	uint16_t x = 0;
	uint16_t y = 0;
	bool known  = false;
	uint16_t record = 0;
	GlyphMask mask = {};
	// Filled in every frame by the first pass: whether the cell could be read
	// on its own and the colours the text on screen is made of. A cell the
	// pointer hides borrows both from the rest of its line.
	bool seen_this_frame = false;
	uint32_t ink         = 0;
	uint32_t background  = 0;
	// Set as soon as the cell could be read on its own on the screen that is up
	// now, and cleared for every cell that a screen change left behind. A record
	// that was never verified on this screen is never painted below the pointer,
	// which is what keeps the characters of the previous screen from returning.
	bool verified        = false;
	bool seen_last_frame = false;
	// Frames in a row in which the cell could not be read although the pointer
	// was nowhere near it. Reaching the limit drops the record for good.
	uint8_t misses       = 0;
};

// Plain text holds three distinct colours: the ink plus up to two shades of a
// dithered background. A hovered or selected entry is redrawn on a highlight
// bar and may carry a cursor or a border as well, so the cell is allowed a few
// more. The glyph itself must still be one single colour whose 224-bit pattern
// indexes exactly one font record, which is what keeps the test safe on
// pictures that are not text at all.
constexpr int MinGlyphInkPixels = 4;
constexpr int MaxGlyphInkPixels = 150;
constexpr int MaxTextCellColors = 6;

// The pointer is a thin outline arrow of roughly 16x24 pixels whose hotspot is
// the reported position, so its body reaches that far to the right and below
// (and a little up and left when the hotspot is not at the very corner). A cell
// is treated as hovered whenever any of that body can reach it.
constexpr int TextPointerMargin = 24;

constexpr int GlyphAtlasColumns = 64;
constexpr int GlyphTileWidth    = TextGlyphWidth * HdScale;
constexpr int GlyphTileHeight   = TextGlyphHeight * HdScale;

struct GlyphAtlas {
	AssetState state = AssetState::Unloaded;
	int width        = 0;
	int height       = 0;
	std::vector<uint8_t> alpha     = {};
	std::vector<uint8_t> available = {};
};

std::deque<TextCell> text_cells = {};
GlyphAtlas glyph_atlas = {};
uint16_t pending_glyph_record = 0;

// Every 28-byte read of the font file reveals one glyph bitmap and its
// record number. The whole 28-byte bitmap is the key, so a lookup either
// finds the exact glyph or nothing at all; there is no hash collision to
// turn one character into another.
struct GlyphMaskHash {
	size_t operator()(const GlyphMask& mask) const noexcept
	{
		uint64_t hash = 1469598103934665603ull;
		for (const auto byte : mask) {
			hash ^= byte;
			hash *= 1099511628211ull;
		}
		return static_cast<size_t>(hash);
	}
};

std::unordered_map<GlyphMask, uint16_t, GlyphMaskHash> glyph_index = {};
// The other way round: the shape of a record, so a cell that knows which glyph
// the renderer blitted can compare itself against that glyph.
std::unordered_map<uint16_t, GlyphMask> record_masks = {};
uint32_t glyph_match_total = 0;
uint32_t glyph_miss_total  = 0;
uint32_t glyph_index_used  = 0;

void reset_glyph_index();

bool hd_enabled = false;
bool trace_enabled = false;
bool frame_dirty = true;
uint16_t main_pspseg = 0;
int trace_event_count = 0;

// The game changes whole screens by rewriting every pixel with a brighter or
// darker value over a dozen frames. A single box cannot tell such a fade from
// artwork, and the average brightness alone cannot either: opening a dialog
// moves it as well. A fade repaints every part of the screen, so the frame is
// compared cell by cell with one a whole window of frames older. Only a change
// that covers most of the screen is a fade. The fade is stepped and each step
// is held for a few frames, hence the window instead of the previous frame.
constexpr int LumaCols        = 32;
constexpr int LumaRows        = 24;
constexpr int LumaCells       = LumaCols * LumaRows;
constexpr int LumaHistorySize = 8;
using LumaGrid = std::array<uint8_t, LumaCells>;
std::array<LumaGrid, LumaHistorySize> luma_history = {};
int luma_history_count = 0;
int luma_history_next  = 0;
bool screen_fading     = false;

// Set for the frames on which the character selection screen is up, that is
// when most of the six fixed portrait boxes are drawn.
bool portrait_scene_active = false;

// Set once the character selection screen is recognised, when the pointer was
// taken out of the game's own framebuffer for this visit of the screen.
bool scene_cursor_suppressed = false;

std::filesystem::path assets_root = "../assets/hd";
std::array<Asset, Kao64EntryCount> assets = {};
std::deque<DrawCommand> draw_commands = {};

// The port screen is a 96x96 grid of 16x16 tiles that the game composes into a
// 1536x1536 map and scrolls through a 384x384 window at (96,48), one whole tile
// at a time. Both the map and the window are known from the game's own
// PORTMAP/PORTCHIP files, so the visible part can be drawn at 4x from a single
// image of the whole map: the offset is found by matching the frame against the
// reconstructed low resolution map, and every pixel the game painted on top of
// the map (the walking character, an item, the pointer) keeps the game's pixels.
constexpr int PortMapStep        = 16;
constexpr int PortMapGridSize    = 96;
constexpr int PortMapSize        = PortMapGridSize * PortMapStep;
constexpr int PortMapWindowX     = 96;
constexpr int PortMapWindowY     = 48;
constexpr int PortMapCells       = 24;
constexpr int PortMapWindowSize  = PortMapCells * PortMapStep;
constexpr int PortMapCellCount   = PortMapCells * PortMapCells;
constexpr int PortMapOffsetSteps = PortMapGridSize - PortMapCells;

// Both the map and the window are grid aligned, so one hash per 16x16 cell
// identifies the picture exactly: a cell either is that tile or it is not, and
// a window full of such cells pins where the map is to the tile.
//
// The window still is the map while most of its cells are, because the
// character, an item or a dialog only spoils the cells they cover.
constexpr int PortMapMinCells = (PortMapCellCount * 45) / 100;

// The game fades a screen in by rewriting the palette entry by entry over a
// handful of frames, so while it does, no colour of the window equals the stored
// map even though the picture is already complete. A palette is a function of
// the colour index, so two pixels that are the same colour stay the same while
// two different ones may become equal: the equality pattern of a cell only ever
// gains pairs during a fade, never loses them. Such a window is located with
// that pattern instead, which needs more of the cells to agree because the
// pattern is coarser than the colours it replaces.
constexpr int PortMapRelationMinCells = (PortMapCellCount * 55) / 100;

// A 16x16 cell has 15x16 right and 16x15 below neighbour pairs.
constexpr int PortMapRightBits  = PortMapStep * (PortMapStep - 1);
constexpr int PortMapRelationBits = 2 * PortMapRightBits;
constexpr int PortMapRelationWords = (PortMapRelationBits + 63) / 64;

// The offset in use is only kept while the window matches it almost entirely.
// The terrain repeats, so an offset that is a tile or two off still agrees with
// about half of the cells; keeping it there would draw the artwork at the wrong
// place and leave the rest of the window in the game's own resolution.
constexpr int PortMapKeepCells = (PortMapCellCount * 90) / 100;

// A picture that repeats all over the map pins nothing, so a window made of
// such cells cannot be located and is left alone.
constexpr size_t PortMapAnchorMax = 64;

// Reconnaissance: how many frames of a window the map only partly describes are
// written out, together with the colours that actually differ.
constexpr int PortMapDiagCount = 8;

// A cell that is only partly the map (the character stands in it) is still
// drawn pixel by pixel, but a cell that mostly shows something else (a dialog,
// a label) is left alone entirely so no map pixel leaks into it.
constexpr int PortMapPartialCell = (PortMapStep * PortMapStep) / 2;

// A message box painted over the port is one flat colour over a large area, and
// that paper is exactly one of the map's own colours - the cream of the sand.
// Read off the frame it therefore looks like a piece of the map, and the
// artwork's outlines would be drawn through the dialog. The map itself is
// dithered, so the very same colour never runs far: a pixel that sits in a long
// run both across and down is paper, not map.
constexpr int PortMapPanelRun = 24;

// The same paper hides a large part of the port, not just a strip of it: a
// message box or a menu can cover more than half of the window, and then neither
// the colours nor the equality pattern of the window describe the map any more
// and the port drops back to the game's own resolution for as long as the panel
// is open. The paper is a flat area and the map never is, so the cells that
// consist of it are recognised and left out of the score: what remains is the
// port, and the port is the map.
constexpr int PortMapPanelMinCells    = PortMapCellCount / 3;
constexpr int PortMapPanelMaxCells    = (PortMapCellCount * 3) / 4;
constexpr int PortMapPanelCellPixels  = (PortMapStep * PortMapStep * 3) / 4;
// The cells on the border of the box are only partly paper, so the very tile
// they hold no longer matches as a whole; the rest of the port does.
constexpr int PortMapPanelKeepPercent = 70;

// The game fades its whole palette out before it opens a building, and on the
// way to black the entries collapse: colours that the stored map keeps apart
// become one, the equality pattern of the cells changes and the port is no
// longer recognised although it is plainly still on screen. The port cannot
// move while the palette fades, so the offset of the frame before is kept. Less
// of the pattern has to hold for that, and only right after a frame that drew
// the port; a building, a shop or another screen does not share the pattern at
// all and is still left alone.
constexpr int PortMapBridgeMinCells = (PortMapCellCount * 45) / 100;

// A screen that went all the way to black holds one colour, and nothing of the
// port can be told apart in it any more.
constexpr int PortMapBridgeMinColours = 3;

// How many of the sampled pixels the mapping has to explain before it is
// trusted to describe a window that was not checked cell by cell.
constexpr int PortMapBridgeExplainedPercent = 80;

// Locating is a handful of cell comparisons now that the hashes exist, so a map
// that could not be found is looked for again on the next frame: the first
// frame of the map screen must not stay in the game's own resolution.
constexpr uint32_t PortMapRetryFrames = 0;

struct PortMap {
	uint32_t index = 0;
	// The reconstructed map, one 0x00RRGGBB per pixel, PortMapSize square.
	std::vector<uint32_t> pixels = {};
	// One hash per cell of the tile grid, plus where each picture repeats, so a
	// single distinctive cell is enough to find the offset.
	std::array<uint64_t, PortMapGridSize * PortMapGridSize> cells = {};
	std::unordered_map<uint64_t, std::vector<uint16_t>> cell_index = {};
	// The same grid, but built from the equality pattern of each cell instead of
	// its colours, so a window whose palette is still fading in is recognised.
	std::array<uint64_t, PortMapGridSize * PortMapGridSize> relation_hashes = {};
	std::unordered_map<uint64_t, std::vector<uint16_t>> relation_index = {};
	// The game recolours the whole port for each time of day. The artwork exists
	// once, for the palette it was drawn with, so it is scaled by the brightness
	// difference between this palette and that one.
	std::array<float, 3> gain = {1.0f, 1.0f, 1.0f};
	// The average colour the artwork has inside each flat colour of this map.
	// The artwork is far lighter there than the game's dithered map - black
	// outlines came out as a pale blue - so drawing it as it is makes the whole
	// port too bright. The average is computed once, from the map and the
	// artwork, and the artwork is then centred on the game's own colour.
	mutable bool art_means_ready = false;
	mutable std::unordered_map<uint32_t, std::array<float, 3>> art_means = {};
};

// The artwork and the brightness of the reference palette are shared by every
// time of day of the same map, because the artwork itself only exists once.
std::unordered_map<uint32_t, std::vector<uint32_t>> port_map_art = {};
std::unordered_map<uint32_t, std::array<float, 3>> port_map_palette = {};

std::vector<PortMap> port_maps = {};
bool port_maps_loaded         = false;
int port_map_selected         = -1;
bool port_map_visible         = false;
int port_map_offset_x         = 0;
int port_map_offset_y         = 0;
uint32_t port_map_retry       = 0;
int port_map_diags            = 0;
int port_map_loss_logs         = 0;
int port_map_loss_dumps        = 0;
int port_map_miss_logs         = 0;
// One byte per pixel of the window: set when the game painted a panel there,
// both across and down; see port_panel_mask().
std::array<uint8_t, PortMapWindowSize * PortMapWindowSize> port_map_panel = {};
bool port_map_panel_ready = false;
// Whether the previous frame drew the port, so a frame that loses it is logged
// once per transition instead of once per frame of a fade.
bool port_map_drew_last = false;
// The game fades the screen out on its way into a building, and the map is still
// there while it does. The window is then a darker copy of the map rather than
// an exact one, and the artwork follows that brightness instead of stepping
// aside for the game's own pixels.
std::array<float, 3> port_map_fade = {1.0f, 1.0f, 1.0f};
int port_map_rel_logs            = 0;
int port_map_fade_logs           = 0;
// Reconnaissance for walking: every change between the artwork and the game's
// own pixels is logged, because such a change is exactly what a flash looks like.
int port_map_state_logs  = 0;
int port_map_last_state  = -1;
// Set when the window was located by the equality pattern of its cells because
// its palette is not one of the stored ones. The map is then drawn through the
// palette read off the frame itself: every colour of the map has exactly one
// colour on screen, so the artwork keeps the screen's colours and only adds the
// detail, whatever the palette has become.
bool port_map_measured = false;
std::vector<std::pair<uint32_t, uint32_t>> port_map_palette_pairs = {};

// Temporary diagnostic sink: the game is launched without a console, so the
// decisions about each portrait box are written next to the working directory.
std::FILE* diag_file = nullptr;
int diag_lines       = 0;
uint32_t composite_count = 0;
int diag_dump_count      = 0;
int diag_hd_dump_count   = 0;
int diag_box_dump_count  = 0;

void diag(const char* format, ...)
{
	if (diag_lines >= 200000) {
		return;
	}
	if (!diag_file) {
		diag_file = std::fopen("sea2-hd-diag.log", "w");
		if (!diag_file) {
			return;
		}
	}
	va_list args;
	va_start(args, format);
	std::vfprintf(diag_file, format, args);
	va_end(args);
	std::fputc('\n', diag_file);
	std::fflush(diag_file);
	++diag_lines;
}

// How many frames in a row the six portrait boxes stopped carrying a picture as
// dense as the frozen one, and whether the screen has already shown a lit frame
// since the last black one.
int scene_lost_frames     = 0;
bool screen_has_been_lit  = false;

// The character selection screen is over: its commands are dropped for good, so
// nothing of that screen can be painted over the next one. The game re-adds the
// commands by itself if the screen is ever entered again.
void drop_scene_commands()
{
	const auto before = draw_commands.size();
	if (before == 0) {
		return;
	}
	draw_commands.erase(std::remove_if(draw_commands.begin(),
	                                   draw_commands.end(),
	                                   [](const DrawCommand& command) {
		                                   return is_scene_portrait_box(command);
	                                   }),
	                    draw_commands.end());
	portrait_scene_active    = false;
	scene_cursor_suppressed  = false;
	scene_lost_frames        = 0;
	diag("SCENEDROP removed=%u frame=%u",
	     static_cast<unsigned>(before - draw_commands.size()),
	     composite_count);
}

// One-off reconnaissance: aggregate executed string instructions by
// (code address, source segment). A glyph blitter stands out because its
// source offset sweeps a wide range inside the font buffer.
struct StringOpSite {
	bool used = false;
	uint16_t cs = 0;
	uint16_t ip = 0;
	uint16_t ds = 0;
	uint32_t count = 0;
	uint32_t min_si = 0xffffffff;
	uint32_t max_si = 0;
	uint32_t min_di = 0xffffffff;
	uint32_t max_di = 0;
	uint8_t opcode = 0;
};

constexpr size_t StringOpTableSize = 16384;
constexpr uint32_t StringOpDumpInterval = 300;
constexpr uint32_t StringOpDumpEveryOps = 2000000;
std::array<StringOpSite, StringOpTableSize> string_op_table = {};
uint32_t string_op_frames = 0;
uint32_t string_op_total = 0;

// Glyph-sized blits land here: the generic planar blitter loads its source
// pointer at IP 0x02B3. Text glyphs are 2 bytes wide and 16 rows tall.
struct GlyphBlitSite {
	bool used = false;
	uint16_t ds = 0;
	uint16_t src = 0;
	uint32_t count = 0;
	uint16_t min_x = 0xffff;
	uint16_t max_x = 0;
	uint16_t min_y = 0xffff;
	uint16_t max_y = 0;
};

constexpr size_t GlyphBlitTableSize = 32768;
std::array<GlyphBlitSite, GlyphBlitTableSize> glyph_blit_table = {};
uint32_t glyph_blit_total = 0;

struct BlitShape {
	bool used = false;
	uint16_t width = 0;
	uint16_t height = 0;
	uint32_t count = 0;
	uint16_t min_src = 0xffff;
	uint16_t max_src = 0;
	uint16_t ds = 0;
	uint16_t min_x = 0xffff;
	uint16_t max_x = 0;
	uint16_t min_y = 0xffff;
	uint16_t max_y = 0;
};

constexpr size_t BlitShapeTableSize = 4096;
std::array<BlitShape, BlitShapeTableSize> blit_shape_table = {};
uint16_t sea2_font_handle = 0xffff;

// Code addresses executed right after a 28-byte font glyph read.
struct GlyphSite {
	bool used = false;
	uint16_t cs = 0;
	uint16_t ip = 0;
	uint32_t count = 0;
};

constexpr size_t GlyphSiteTableSize = 8192;
std::array<GlyphSite, GlyphSiteTableSize> glyph_site_table = {};

// Return addresses found on the stack when a 28-byte font glyph is read.
struct GlyphCaller {
	bool used = false;
	uint16_t cs = 0;
	uint16_t ip = 0;
	uint32_t count = 0;
};

constexpr size_t GlyphCallerTableSize = 4096;
std::array<GlyphCaller, GlyphCallerTableSize> glyph_caller_table = {};

// Raw stack window captured on the first glyph reads, so the real call chain
// can be reconstructed without guessing the frame layout.
struct StackSample {
	uint16_t ss = 0;
	uint16_t sp = 0;
	uint16_t ax = 0;
	uint16_t bx = 0;
	uint16_t cx = 0;
	uint16_t dx = 0;
	uint16_t ds = 0;
	std::array<uint16_t, 16> words = {};
};

constexpr size_t StackSampleCount = 64;
std::array<StackSample, StackSampleCount> stack_samples = {};
size_t stack_sample_total = 0;

// Bytes at the caller address captured while the read is happening, so later
// memory changes cannot hide the instruction that performed the call.
std::array<std::array<uint8_t, 32>, StackSampleCount> caller_bytes = {};

// Execution history captured right before the DOS read wrapper executes its
// INT 21h, which shows the real caller without relying on frame layout.
struct Int21Sample {
	uint16_t ss = 0;
	uint16_t sp = 0;
	uint16_t bx = 0;
	uint16_t cx = 0;
	uint16_t dx = 0;
	std::array<uint32_t, 48> history = {};
};

constexpr size_t Int21SampleCount = 8;
std::array<Int21Sample, Int21SampleCount> int21_samples = {};
size_t int21_sample_total = 0;

// Raw arguments captured at the two text blitter entry points selected by
// the far-call table at DS:0x8A78.
struct BlitterSample {
	uint16_t ip = 0;
	uint16_t bp = 0;
	uint16_t ax = 0;
	uint16_t bx = 0;
	uint16_t cx = 0;
	uint16_t dx = 0;
	uint16_t si = 0;
	uint16_t di = 0;
	uint16_t es = 0;
	uint16_t ds = 0;
	std::array<uint16_t, 10> args = {};
	std::array<uint8_t, 32> glyph = {};
};

constexpr size_t BlitterSampleCount = 64;
std::array<BlitterSample, BlitterSampleCount> blitter_samples = {};
size_t blitter_sample_total = 0;
size_t blitter_entry_total = 0;

constexpr uint16_t BlitterEntryA = 0x0636;
constexpr uint16_t BlitterEntryB = 0x0653;

// Callers of the BIOS keyboard service, aggregated by code address. The
// password screen polls the keyboard from a small input loop.
struct KeySite {
	bool used = false;
	uint16_t cs = 0;
	uint16_t ip = 0;
	uint32_t count = 0;
	uint8_t last_ah = 0;
	uint16_t last_ax = 0;
};

constexpr size_t KeySiteTableSize = 4096;
std::array<KeySite, KeySiteTableSize> key_site_table = {};
uint32_t key_site_total = 0;

// Callers of individual DOS services, keyed by function number.
struct DosCallSite {
	bool used = false;
	uint16_t cs = 0;
	uint16_t ip = 0;
	uint8_t ah = 0;
	uint32_t count = 0;
};

constexpr size_t DosCallTableSize = 8192;
std::array<DosCallSite, DosCallTableSize> dos_call_table = {};
uint32_t dos_call_total = 0;

// Sampled instruction addresses: a coarse profile that exposes the idle
// polling loop sitting on the password screen.
struct HotSite {
	bool used = false;
	uint16_t cs = 0;
	uint16_t ip = 0;
	uint32_t count = 0;
};

constexpr size_t HotSiteTableSize = 16384;
std::array<HotSite, HotSiteTableSize> hot_site_table = {};
uint32_t hot_site_total = 0;

// Stack frame seen when the password screen routine is entered.
struct EntrySample {
	uint16_t cs = 0;
	uint16_t ip = 0;
	uint16_t ss = 0;
	uint16_t sp = 0;
	std::array<uint16_t, 12> words = {};
};

constexpr size_t EntrySampleCount = 32;
std::array<EntrySample, EntrySampleCount> entry_samples = {};
size_t entry_sample_total = 0;

// 16x16 glyph bitmaps copied from 2.PAT; used to locate the resident font.
struct FontSignature {
	uint16_t glyph;
	std::array<uint8_t, 32> bytes;
};

constexpr size_t FontSignatureCount = 5;
const std::array<FontSignature, FontSignatureCount> font_signatures = {{
        {0,
         {0x00, 0x08, 0x10, 0x08, 0x10, 0x08, 0x08, 0x10, 0x04, 0x10, 0x02, 0x20,
          0x01, 0x40, 0x00, 0x80, 0x01, 0x40, 0x02, 0x20, 0x04, 0x10, 0x18, 0x0c,
          0x60, 0x03, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00}},
        {1700,
         {0x13, 0xfe, 0x10, 0xa8, 0x11, 0x24, 0x12, 0x22, 0x14, 0x21, 0x10, 0x20,
          0x10, 0x48, 0x10, 0x48, 0x13, 0xcf, 0x18, 0x48, 0x54, 0x48, 0x53, 0xcf,
          0x50, 0x48, 0x10, 0x48, 0x11, 0xcf, 0x16, 0x48}},
        {3400,
         {0x24, 0x44, 0x7e, 0x7c, 0x24, 0x10, 0x24, 0xfe, 0x24, 0x92, 0x24, 0xfe,
          0x48, 0x10, 0x30, 0xfe, 0x18, 0x20, 0x25, 0xff, 0x40, 0x44, 0x01, 0x82,
          0x20, 0x00, 0x23, 0xff, 0x2a, 0x00, 0x7a, 0x44}},
        {5100,
         {0x7f, 0x28, 0x0a, 0x28, 0x29, 0x44, 0x48, 0x83, 0x00, 0x80, 0x7f, 0xff,
          0x42, 0x21, 0x44, 0x39, 0x00, 0x00, 0x3e, 0xfe, 0x08, 0x82, 0x08, 0xfe,
          0x2e, 0x82, 0x28, 0xfe, 0x28, 0x28, 0x2e, 0x49}},
        {6699,
         {0x4d, 0x2f, 0x71, 0xea, 0x04, 0x0f, 0x12, 0xaa, 0x49, 0x5f, 0x41, 0x08,
          0x10, 0x84, 0x23, 0xe8, 0x68, 0x1a, 0x13, 0xe4, 0x25, 0x49, 0x7d, 0xdf,
          0x54, 0x15, 0x54, 0x95, 0x3f, 0xfe, 0x20, 0x82}},
}};


template <size_t N>
bool signature_matches(uint16_t loadseg, uint16_t ip,
                       const std::array<uint8_t, N>& signature)
{
	for (size_t i = 0; i < N; ++i) {
		if (i == 20 || i == 21) {
			continue;
		}
		const auto address = PhysicalMake(loadseg, static_cast<uint16_t>(ip + i));
		if (mem_readb(address) != signature[i]) {
			return false;
		}
	}
	return true;
}

bool main_entrypoints_match(const uint16_t loadseg)
{
	return signature_matches(loadseg, Kao64Entry, Kao64Signature) &&
	       signature_matches(loadseg, Kao48Entry, Kao48Signature);
}

void reset_assets()
{
	for (auto& asset : assets) {
		asset = {};
	}
	glyph_atlas = {};
	text_cells.clear();
	pending_glyph_record = 0;
	reset_glyph_index();
	port_maps.clear();
	port_map_art.clear();
	port_map_palette.clear();
	port_maps_loaded = false;
	port_map_selected = -1;
	port_map_visible = false;
	port_map_fade = {1.0f, 1.0f, 1.0f};
	port_map_rel_logs = 0;
	port_map_fade_logs = 0;
	port_map_state_logs = 0;
	port_map_last_state = -1;
	port_map_measured = false;
	port_map_palette_pairs.clear();
	port_map_retry = 0;
	port_map_diags = 0;
	port_map_loss_logs = 0;
	port_map_loss_dumps = 0;
	port_map_miss_logs = 0;
	port_map_panel_ready = false;
	port_map_drew_last = false;
}

const Asset* get_kao64_asset(const uint16_t entry)
{
	assert(entry < assets.size());
	auto& asset = assets[entry];
	if (asset.state == AssetState::Loaded) {
		return &asset;
	}
	if (asset.state == AssetState::Missing) {
		return nullptr;
	}

	// A missing or invalid asset is remembered so we never retry disk I/O on
	// every frame. The untouched 4x source frame remains as the fallback.
	asset.state = AssetState::Missing;

	char filename[16] = {};
	std::snprintf(filename, sizeof(filename), "%03u.png", entry);
	const auto path = assets_root / "KAO" / "portraits" / filename;
	const auto path_string = path.string();

	auto* loaded = IMG_Load(path_string.c_str());
	if (!loaded) {
		diag("ASSETMISS entry=%u path=%s", entry, path_string.c_str());
		LOG_WARNING("SEA2HD: Cannot load KAO entry %u: %s", entry, IMG_GetError());
		return nullptr;
	}

	auto* converted = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
	SDL_FreeSurface(loaded);
	if (!converted) {
		LOG_WARNING("SEA2HD: Cannot decode KAO entry %u as RGBA: %s",
		            entry,
		            SDL_GetError());
		return nullptr;
	}

	if (converted->w != Kao64Width * HdScale ||
	    converted->h != Kao64Height * HdScale) {
		diag("ASSETDIM entry=%u w=%d h=%d", entry, converted->w, converted->h);
		LOG_WARNING("SEA2HD: KAO entry %u has invalid dimensions %dx%d",
		            entry,
		            converted->w,
		            converted->h);
		SDL_FreeSurface(converted);
		return nullptr;
	}

	if (SDL_LockSurface(converted) != 0) {
		LOG_WARNING("SEA2HD: Cannot lock KAO entry %u: %s", entry, SDL_GetError());
		SDL_FreeSurface(converted);
		return nullptr;
	}

	asset.rgba.resize(static_cast<size_t>(converted->w) * converted->h * 4);
	for (int y = 0; y < converted->h; ++y) {
		const auto* src = static_cast<const uint8_t*>(converted->pixels) +
		                  y * converted->pitch;
		std::memcpy(asset.rgba.data() + static_cast<size_t>(y) * converted->w * 4,
		            src,
		            static_cast<size_t>(converted->w) * 4);
	}

	SDL_UnlockSurface(converted);
	SDL_FreeSurface(converted);

	asset.state = AssetState::Loaded;
	LOG_MSG("SEA2HD: Loaded KAO entry %u as 256x320 RGBA", entry);
	return &asset;
}

// Every portrait file is decoded once, when the game itself starts. Decoding on
// first use puts the disk read and the PNG decode into the frame that first
// shows the portrait, which costs a visible moment on the character selection
// screen; doing it up front leaves that screen untouched. The files are found
// through the directory, so an absent HD portrait is not even looked for.
void preload_kao64_assets()
{
	const auto dir = assets_root / "KAO" / "portraits";
	std::error_code error;
	const auto started = SDL_GetTicks64();

	auto loaded = 0u;
	for (const auto& file : std::filesystem::directory_iterator(dir, error)) {
		const auto name = file.path().filename().string();
		if (name.size() != 7 || name.compare(3, 4, ".png") != 0) {
			continue;
		}
		int entry  = 0;
		bool valid = true;
		for (auto i = 0; i < 3; ++i) {
			const auto digit = name[static_cast<size_t>(i)];
			if (digit < '0' || digit > '9') {
				valid = false;
				break;
			}
			entry = entry * 10 + (digit - '0');
		}
		if (!valid || entry >= Kao64EntryCount) {
			continue;
		}
		if (get_kao64_asset(static_cast<uint16_t>(entry))) {
			++loaded;
		}
	}

	if (error) {
		LOG_WARNING("SEA2HD: Cannot scan KAO portraits in %s: %s",
		            dir.string().c_str(),
		            error.message().c_str());
		return;
	}
	LOG_MSG("SEA2HD: Preloaded %u KAO portraits in %llu ms",
	        loaded,
	        static_cast<unsigned long long>(SDL_GetTicks64() - started));
}

void record_kao64_command()
{
	// The original blitter rejects the draw unless the complete 64x80 image
	// fits in its 640x400 content area. AX is a VGA byte-x coordinate.
	if (reg_bx >= assets.size() || reg_ax > VgaWidth / 8 - Kao64Width / 8 ||
	    reg_dx > VgaHeight - ContentYOffset - Kao64Height) {
		diag("REJECT entry=%u ax=%u dx=%u", reg_bx, reg_ax, reg_dx);
		return;
	}

	DrawCommand command = {};
	command.entry = reg_bx;
	command.x = static_cast<uint16_t>(reg_ax * 8);
	command.y = reg_dx;

	for (auto it = draw_commands.begin(); it != draw_commands.end();) {
		if (it->x != command.x || it->y != command.y) {
			++it;
			continue;
		}
		// The game blits the same portrait into the same box over and over.
		// Keeping the command keeps the first reference; rebuilding it would
		// hand the box's current contents another chance to become the
		// reference, including the frame that erases the window.
		if (it->entry == command.entry) {
			frame_dirty = true;
			return;
		}
		diag("SWAP x=%u y=%u old=%u new=%u", command.x, command.y, it->entry,
		     command.entry);
		it = draw_commands.erase(it);
	}
	if (draw_commands.size() == MaxDrawCommands) {
		draw_commands.pop_front();
	}
	draw_commands.push_back(std::move(command));

	// The character selection screen is the only screen that blits portrait
	// after portrait into the six fixed boxes. The pointer is taken out of the
	// game's own framebuffer right there - before any box is compared - which
	// is what the screen looks like when it is entered from the keyboard. The
	// driver brings the pointer back as soon as the mouse is moved.
	if (!scene_cursor_suppressed && is_scene_portrait_box(draw_commands.back())) {
		int scene_boxes = 0;
		for (const auto& other : draw_commands) {
			if (is_scene_portrait_box(other)) {
				++scene_boxes;
			}
		}
		if (scene_boxes >= KaoSceneMinIntactBoxes) {
			scene_cursor_suppressed = true;
			MOUSEDOS_SuppressCursorUntilMove();
			diag("HIDE pointerscene boxes=%d frame=%u", scene_boxes,
			     composite_count);
		}
	}

	diag("ADD entry=%u x=%u y=%u count=%zu",
	     draw_commands.back().entry,
	     draw_commands.back().x,
	     draw_commands.back().y,
	     draw_commands.size());
	frame_dirty = true;
}

const uint32_t* kao_box_origin(const DrawCommand& command, const uint32_t* source,
                               const int source_width)
{
	return source +
	       static_cast<size_t>(command.y + ContentYOffset) * source_width + command.x;
}

struct RelationChange {
	int equal_to_different    = 0;
	int different_to_equal    = 0;

	int total() const
	{
		return equal_to_different + different_to_equal;
	}
};

RelationChange compare_relation_bits(
        const std::array<uint8_t, KaoRelationBytes>& reference,
        const std::array<uint8_t, KaoRelationBytes>& current)
{
	RelationChange change = {};
	for (size_t i = 0; i < reference.size(); ++i) {
		const auto ref_byte = reference[i];
		const auto cur_byte = current[i];
		for (auto diff = static_cast<uint8_t>(ref_byte ^ cur_byte); diff != 0;
		     diff &= static_cast<uint8_t>(diff - 1)) {
			// The lowest set bit of `diff` names the pair that disagrees.
			const auto mask = static_cast<uint8_t>(diff & (~diff + 1));
			(0 != (ref_byte & mask) ? change.different_to_equal
			                        : change.equal_to_different) += 1;
		}
	}
	return change;
}

uint32_t channel_sum(const uint32_t pixel)
{
	return ((pixel >> 16) & 0xff) + ((pixel >> 8) & 0xff) + (pixel & 0xff);
}

void capture_box_pixels(const uint32_t* region, const int stride,
                        DrawCommand& command)
{
	for (int row = 0; row < Kao64Height; ++row) {
		const auto* line = region + static_cast<size_t>(row) * stride;
		auto* target = command.pixels.data() + static_cast<size_t>(row) * Kao64Width;
		std::copy(line, line + Kao64Width, target);
	}
	command.pixels_valid = true;
}

// Least-squares fit of the stored picture onto the current frame, per channel:
// current = gain * stored + offset. Accumulated over pixels, solved afterwards.
struct FitSums {
	std::array<uint64_t, 3> stored     = {};
	std::array<uint64_t, 3> actual     = {};
	std::array<uint64_t, 3> stored_sq  = {};
	std::array<uint64_t, 3> stored_act = {};
	uint64_t samples                   = 0;
};

struct ChannelFit {
	double gain   = 1.0;
	double offset = 0.0;
};

template <typename Accept>
void accumulate_fit(const uint32_t* ref, const uint32_t* line, const int width,
                    Accept&& accept, FitSums& sums)
{
	for (int col = 0; col < width; ++col) {
		if (!accept(ref[col], line[col])) {
			continue;
		}
		for (auto channel = 0; channel < 3; ++channel) {
			const auto shift  = channel * 8;
			const auto stored = (ref[col] >> shift) & 0xff;
			const auto actual = (line[col] >> shift) & 0xff;
			sums.stored[channel] += stored;
			sums.actual[channel] += actual;
			sums.stored_sq[channel] += static_cast<uint64_t>(stored) * stored;
			sums.stored_act[channel] += static_cast<uint64_t>(stored) * actual;
		}
		++sums.samples;
	}
}

std::array<ChannelFit, 3> solve_fit(const FitSums& sums)
{
	std::array<ChannelFit, 3> fit = {};
	if (sums.samples == 0) {
		return fit;
	}
	const auto sample_count = static_cast<double>(sums.samples);
	for (auto channel = 0; channel < 3; ++channel) {
		double gain = 1.0;
		const auto spread = sample_count *
		                            static_cast<double>(sums.stored_sq[channel]) -
		                    static_cast<double>(sums.stored[channel]) *
		                            static_cast<double>(sums.stored[channel]);
		if (spread > 1.0) {
			gain = (sample_count * static_cast<double>(sums.stored_act[channel]) -
			        static_cast<double>(sums.stored[channel]) *
			                static_cast<double>(sums.actual[channel])) /
			       spread;
			gain = std::clamp(gain, 0.0, 4.0);
		}
		auto offset = (static_cast<double>(sums.actual[channel]) -
		               gain * static_cast<double>(sums.stored[channel])) /
		              sample_count;
		fit[channel] = {gain, std::clamp(offset, -255.0, 255.0)};
	}
	return fit;
}

double pixel_deviation(const uint32_t stored, const uint32_t actual,
                       const std::array<ChannelFit, 3>& fit)
{
	double difference = 0.0;
	for (auto channel = 0; channel < 3; ++channel) {
		const auto shift = channel * 8;
		const auto ref   = (stored >> shift) & 0xff;
		const auto cur   = (actual >> shift) & 0xff;
		auto deviation = fit[channel].gain * static_cast<double>(ref) +
		                 fit[channel].offset - static_cast<double>(cur);
		if (deviation < 0.0) {
			deviation = -deviation;
		}
		difference += deviation;
	}
	return difference;
}

// Pixels of the box that no longer belong to the stored picture, marked one by
// one, plus whether they form a solid block. A window, a label or the pointer is
// not part of the brightness fit and still stands out; a fade is absorbed by it.
int mark_foreign_pixels(
        const uint32_t* region, const int stride,
        const std::array<uint8_t, KaoRelationBytes>& current, DrawCommand& command)
{
	command.covered.fill(0);
	command.overlay_like = false;
	if (!command.pixels_valid) {
		return 0;
	}

	FitSums all_sums = {};
	for (int row = 0; row < Kao64Height; ++row) {
		const auto* line = region + static_cast<size_t>(row) * stride;
		const auto* ref = command.pixels.data() +
		                  static_cast<size_t>(row) * Kao64Width;
		accumulate_fit(ref, line, Kao64Width,
		               [](uint32_t, uint32_t) { return true; }, all_sums);
	}
	if (all_sums.samples == 0) {
		return 0;
	}
	const auto current_total   = all_sums.actual[0] + all_sums.actual[1] +
	                             all_sums.actual[2];
	const auto reference_total = all_sums.stored[0] + all_sums.stored[1] +
	                             all_sums.stored[2];
	if (reference_total == 0 || current_total == 0) {
		// A cleared or black box holds nothing of the picture at all.
		command.covered.fill(1);
		return Kao64Width * Kao64Height;
	}

	// current = gain * stored + offset, per channel. An unfinished fade or a box
	// whose pixels were copied while the game was still drawing it is exactly
	// such a transform; a window or a dialog is not.
	using BrightnessFit = std::array<ChannelFit, 3>;

	const auto solve_brightness = [&all_sums, region, stride, &command]() {
		BrightnessFit mapping = solve_fit(all_sums);
		FitSums inlier_sums = {};
		for (int row = 0; row < Kao64Height; ++row) {
			const auto* line = region + static_cast<size_t>(row) * stride;
			const auto* ref  = command.pixels.data() +
			                  static_cast<size_t>(row) * Kao64Width;
			accumulate_fit(
			        ref, line, Kao64Width,
			        [&mapping](const uint32_t stored, const uint32_t actual) {
				        return pixel_deviation(stored, actual, mapping) <=
				               KaoFitInlierTolerance;
			        },
			        inlier_sums);
		}
		if (static_cast<uint64_t>(KaoMinFitInlierRatio) * all_sums.samples <=
		    inlier_sums.samples * 100) {
			mapping = solve_fit(inlier_sums);
		}
		return mapping;
	};

	const auto mask_with = [region, stride, &command](const BrightnessFit& mapping) {
		command.covered.fill(0);
		int foreign = 0;
		// A window or a dialog is drawn as a solid block, so the pixels that do
		// not belong to the picture form one long run inside a row. Another
		// picture scatters its mismatches all over the box instead.
		int row_longest_total = 0;
		for (int row = 0; row < Kao64Height; ++row) {
			const auto* line = region + static_cast<size_t>(row) * stride;
			const auto* ref  = command.pixels.data() +
			                  static_cast<size_t>(row) * Kao64Width;
			auto* marks = command.covered.data() +
			              static_cast<size_t>(row) * Kao64Width;
			int run = 0, longest = 0;
			int row_foreign = 0, first = -1, last = -1;
			for (int col = 0; col < Kao64Width; ++col) {
				if (pixel_deviation(ref[col], line[col], mapping) >
				    KaoForeignPixelTolerance) {
					marks[col] = 1;
					++foreign;
					++run;
					++row_foreign;
					if (first < 0) {
						first = col;
					}
					last = col;
					if (run > longest) {
						longest = run;
					}
				} else {
					run = 0;
				}
			}
			// A dialog or a text panel has gaps in its own block: its blank paper
			// can be the very same colour as the face behind it, and the artwork
			// would then be painted over the panel at exactly those spots. Inside
			// a row that is taken up by such a block by a quarter or more,
			// everything between its first and last foreign pixel is left to the
			// game as well.
			if (row_foreign * 4 >= Kao64Width) {
				for (int col = first; col <= last; ++col) {
					marks[col] = 1;
				}
			}
			row_longest_total += longest;
		}
		command.overlay_like =
		        foreign > 0 &&
		        row_longest_total * 100 >= foreign * KaoOverlayRunRatio;
		return foreign;
	};

	// The mask is always measured with the identity first. Without a fade the
	// picture is stored exactly as it is drawn, so every pixel that differs is
	// genuinely something else. Fitting unconditionally is what used to break
	// dialogs: a large uniform block - a dialog, a window - collapses the whole
	// fit onto its own colour (gain near zero), the portrait becomes the
	// "foreign" part, and the artwork ends up painted exactly over the dialog.
	BrightnessFit fit = {};
	if (screen_fading) {
		// The whole screen is being rescaled, so the mapping is the fade itself.
		fit = solve_brightness();
		// A dialog can sit on the box during a fade too, and it would collapse
		// the mapping. A real fade never crushes the picture to a fraction of
		// itself in one frame, so the last good mapping is kept instead.
		if (fit[0].gain < 0.25 && fit[1].gain < 0.25 && fit[2].gain < 0.25) {
			for (auto channel = 0; channel < 3; ++channel) {
				fit[channel].gain =
				        static_cast<double>(command.fade_gain[channel]);
				fit[channel].offset =
				        static_cast<double>(command.fade_offset[channel]);
			}
		}
	}

	int foreign = mask_with(fit);

	if (!screen_fading &&
	    foreign * 10 >= Kao64Width * Kao64Height * 9) {
		// Nothing of the stored pixels matches, which a dialog cannot cause - it
		// never takes the whole box. The pixels are then a copy of the picture
		// taken while the game was still drawing it, so they are at another
		// brightness. A mapping that keeps the picture's contrast and explains
		// most of the box proves it is the very same picture.
		const auto alternative = solve_brightness();
		if (alternative[0].gain >= 0.5 && alternative[1].gain >= 0.5 &&
		    alternative[2].gain >= 0.5) {
			const int alternative_foreign = mask_with(alternative);
			if (alternative_foreign * 4 < foreign * 3) {
				fit     = alternative;
				foreign = alternative_foreign;
			} else {
				mask_with(fit);
			}
		}
	}

	// A panel painted over the picture leaves the rest of the box exactly as it
	// was, so every pair of neighbouring pixels outside the mask still relates
	// the way it did in the stored picture. A different picture leaves nothing
	// intact: the pixels that happen to match are scattered all over the box and
	// the pairs around them no longer relate the same way. That is what tells a
	// window sitting on the portrait apart from another portrait, so the box may
	// be drawn with the mask in the first case and must be dropped in the second.
	int free_pairs   = 0;
	int intact_pairs = 0;
	int bit          = 0;
	for (int row = 0; row < Kao64Height; ++row) {
		const auto* marks = command.covered.data() +
		                    static_cast<size_t>(row) * Kao64Width;
		for (int col = 0; col < Kao64Width - 1; ++col, ++bit) {
			if (marks[col] != 0 || marks[col + 1] != 0) {
				continue;
			}
			++free_pairs;
			const auto bit_mask = static_cast<uint8_t>(0x80u >> (bit & 7));
			if (((command.reference[bit >> 3] ^ current[bit >> 3]) & bit_mask) ==
			    0) {
				++intact_pairs;
			}
		}
	}
	for (int row = 0; row < Kao64Height - 1; ++row) {
		const auto* marks = command.covered.data() +
		                    static_cast<size_t>(row) * Kao64Width;
		const auto* below = marks + Kao64Width;
		for (int col = 0; col < Kao64Width; ++col, ++bit) {
			if (marks[col] != 0 || below[col] != 0) {
				continue;
			}
			++free_pairs;
			const auto bit_mask = static_cast<uint8_t>(0x80u >> (bit & 7));
			if (((command.reference[bit >> 3] ^ current[bit >> 3]) & bit_mask) ==
			    0) {
				++intact_pairs;
			}
		}
	}
	const bool panel_on_intact_picture =
	        free_pairs >= Kao64Width * 4 && intact_pairs * 10 >= free_pairs * 9;
	if (panel_on_intact_picture) {
		command.overlay_like = true;
	}
	if (composite_count % 60 == 0 && free_pairs > 0) {
		diag("PANELTST entry=%u x=%u y=%u frame=%u foreign=%d free=%d intact=%d panel=%d",
		     command.entry, command.x, command.y, composite_count, foreign,
		     free_pairs, intact_pairs, panel_on_intact_picture ? 1 : 0);
	}

	// Remembered so a fade can be applied to the high resolution artwork as
	// well, instead of leaving it lit while the rest of the screen dims.
	for (auto channel = 0; channel < 3; ++channel) {
		command.fade_gain[channel]   = static_cast<float>(fit[channel].gain);
		command.fade_offset[channel] = static_cast<float>(fit[channel].offset);
	}
	return foreign;
}

int count_relation_differences(const std::array<uint8_t, KaoRelationBytes>& a,
                               const std::array<uint8_t, KaoRelationBytes>& b)
{
	int differences = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		for (auto diff = static_cast<uint8_t>(a[i] ^ b[i]); diff != 0;
		     diff &= static_cast<uint8_t>(diff - 1)) {
			++differences;
		}
	}
	return differences;
}

void build_relation_bits(const uint32_t* region, const int stride,
                         std::array<uint8_t, KaoRelationBytes>& bits)
{
	bits.fill(0);
	int bit = 0;
	for (int row = 0; row < Kao64Height; ++row) {
		const auto* line = region + static_cast<size_t>(row) * stride;
		for (int col = 0; col < Kao64Width - 1; ++col, ++bit) {
			if (line[col] == line[col + 1]) {
				bits[bit >> 3] |= static_cast<uint8_t>(0x80u >> (bit & 7));
			}
		}
	}
	for (int row = 0; row < Kao64Height - 1; ++row) {
		const auto* line  = region + static_cast<size_t>(row) * stride;
		const auto* below = line + stride;
		for (int col = 0; col < Kao64Width; ++col, ++bit) {
			if (line[col] == below[col]) {
				bits[bit >> 3] |= static_cast<uint8_t>(0x80u >> (bit & 7));
			}
		}
	}
}

int count_relation_structure(const std::array<uint8_t, KaoRelationBytes>& bits)
{
	int set_bits = 0;
	for (const auto byte : bits) {
		for (auto value = byte; value != 0; value &= static_cast<uint8_t>(value - 1)) {
			++set_bits;
		}
	}
	return KaoRelationBits - set_bits;
}

// Hands back every pixel that belongs to a panel painted over the box - the
// character information window and its "yes/no" buttons, for instance. Such a
// panel is one flat colour that runs far past the box, so a stretch of a single
// colour crossing the left or the right edge names it without knowing where the
// game put it. Everything between the outermost of those stretches in a row is
// handed back as well, because the labels and the buttons inside a panel break
// the paper into pieces that no longer leave the box on their own.
//
// The colour of a portrait never runs past the box: the picture ends with the
// box. A colour that does is therefore a panel, and the mouse pointer, which is
// small and keeps every stretch inside the box, leaves the test untouched.
int mark_panel_pixels(const uint32_t* region, const int stride, const int box_x,
                      DrawCommand& command)
{
	const int left  = std::min(box_x, KaoPanelProbe);
	const int right = std::min(stride - (box_x + Kao64Width), KaoPanelProbe);
	if (left <= 0 && right <= 0) {
		return 0;
	}

	const int from = -left;
	const int to   = (Kao64Width - 1) + right;

	int marked = 0;
	for (int row = 0; row < Kao64Height; ++row) {
		const auto* line = region + static_cast<size_t>(row) * stride;
		auto* marks      = command.covered.data() +
		              static_cast<size_t>(row) * Kao64Width;

		int first = Kao64Width;
		int last  = -1;
		int start = from;
		for (int col = from + 1; col <= to + 1; ++col) {
			if (col <= to && line[col] == line[start]) {
				continue;
			}
			const int run_begin = start;
			const int run_end   = col - 1;
			start               = col;

			// The stretch has to touch the box at all ...
			if (run_end < 0 || run_begin >= Kao64Width) {
				continue;
			}
			// ... and it has to leave it on one side to be a panel.
			if (run_begin > -KaoPanelReach &&
			    run_end < (Kao64Width - 1) + KaoPanelReach) {
				continue;
			}
			first = std::min(first, run_begin);
			last  = std::max(last, run_end);
		}
		if (last < 0) {
			continue;
		}
		for (int col = std::max(first, 0); col <= std::min(last, Kao64Width - 1);
		     ++col) {
			if (marks[col] == 0) {
				marks[col] = 1;
				++marked;
			}
		}
	}
	return marked;
}

// The small confirmation box the game drops next to the pointer. Its paper is
// the very colour of the picture in places, so the pixel test lets those spots
// through and the artwork is painted over the box there. What still gives the
// box away is that the paper is a single colour over a long stretch, while the
// stored picture has texture in exactly those pixels: a flat part of the
// portrait itself reproduces the stored colours, because it is the same
// picture, and is therefore never marked.
int mark_flat_panel_pixels(const uint32_t* region, const int stride,
                           DrawCommand& command)
{
	if (!command.pixels_valid) {
		return 0;
	}

	std::array<uint8_t, Kao64Width * Kao64Height> candidate = {};
	int candidate_total                                     = 0;

	for (int row = 0; row < Kao64Height; ++row) {
		const auto* line = region + static_cast<size_t>(row) * stride;
		const auto* ref  = command.pixels.data() +
		                  static_cast<size_t>(row) * Kao64Width;
		auto* marks = candidate.data() + static_cast<size_t>(row) * Kao64Width;

		int start = 0;
		for (int col = 1; col <= Kao64Width; ++col) {
			if (col < Kao64Width && line[col] == line[start]) {
				continue;
			}
			const int length = col - start;
			if (length >= KaoFlatPanelRun) {
				int differing = 0;
				for (int index = start; index < col; ++index) {
					if (ref[index] != line[start]) {
						++differing;
					}
				}
				// Half of the stored pixels have to disagree with the flat
				// stretch: the paper is new, the picture underneath was not
				// flat there.
				if (differing * 2 >= length) {
					for (int index = start; index < col; ++index) {
						marks[index] = 1;
						++candidate_total;
					}
				}
			}
			start = col;
		}
	}

	// A handful of coincidental pixels is not a box.
	if (candidate_total < KaoFlatPanelMinArea) {
		return 0;
	}

	int marked = 0;
	for (int index = 0; index < Kao64Width * Kao64Height; ++index) {
		if (candidate[index] != 0 && command.covered[index] == 0) {
			command.covered[index] = 1;
			++marked;
		}
	}
	return marked;
}

// Pixels that the game painted over since the box last held the untouched
// picture, measured on the equality pattern instead of on the colours. A fade,
// a dither and a plain redraw change the colours but keep every pair of
// neighbouring pixels in the same relation, so the pattern still names the
// portrait; whatever is drawn on top breaks it, and both pixels of a broken pair
// are handed back to the game.
int mark_changed_pixels(const uint32_t* region, const int stride,
                        const std::array<uint8_t, KaoRelationBytes>& reference,
                        const std::array<uint8_t, KaoRelationBytes>& current,
                        DrawCommand& command)
{
	command.covered.fill(0);
	command.overlay_like = false;

	int bit = 0;
	int changed_bits = 0;
	for (int row = 0; row < Kao64Height; ++row) {
		auto* marks = command.covered.data() +
		              static_cast<size_t>(row) * Kao64Width;
		for (int col = 0; col < Kao64Width - 1; ++col, ++bit) {
			const auto bit_mask = static_cast<uint8_t>(0x80u >> (bit & 7));
			if ((reference[bit >> 3] ^ current[bit >> 3]) & bit_mask) {
				marks[col]     = 1;
				marks[col + 1] = 1;
				++changed_bits;
			}
		}
	}
	for (int row = 0; row < Kao64Height - 1; ++row) {
		auto* marks = command.covered.data() +
		              static_cast<size_t>(row) * Kao64Width;
		auto* below = marks + Kao64Width;
		for (int col = 0; col < Kao64Width; ++col, ++bit) {
			const auto bit_mask = static_cast<uint8_t>(0x80u >> (bit & 7));
			if ((reference[bit >> 3] ^ current[bit >> 3]) & bit_mask) {
				marks[col] = 1;
				below[col] = 1;
				++changed_bits;
			}
		}
	}

	// A pointer drawn with an exclusive-or mask keeps every pair of neighbouring
	// pixels in the same relation (a^c == b^c whenever a == b), so the test above
	// only outlines it while its inside stays hidden. A dialog that happens to sit
	// on a flat part of the picture - the frame of a portrait, for instance -
	// hides the very same way. The stored pixels name the difference directly, so
	// every pixel whose colour no longer matches them is handed back as well.
	const std::array<ChannelFit, 3> identity = {};
	std::array<uint8_t, Kao64Width * Kao64Height> foreign = {};
	int foreign_total = 0;
	if (command.pixels_valid) {
		for (int row = 0; row < Kao64Height; ++row) {
			auto* flags      = foreign.data() + static_cast<size_t>(row) * Kao64Width;
			const auto* line = region + static_cast<size_t>(row) * stride;
			const auto* ref  = command.pixels.data() +
			                   static_cast<size_t>(row) * Kao64Width;
			for (int col = 0; col < Kao64Width; ++col) {
				flags[col] = pixel_deviation(ref[col], line[col], identity) >
				                     KaoForeignPixelTolerance
				                     ? 1
				                     : 0;
				foreign_total += flags[col];
			}
		}
	}
	// The stored pixels can be a copy of this very picture taken while the screen
	// was still fading in: the shapes agree - the pattern test above found almost
	// nothing - while the colours are off by a constant factor, so the pixel test
	// would flag most of the box. Such a copy names nothing that was painted on
	// top, and it can never be renewed the usual way that requires a perfect
	// match, so the portrait would stay half low resolution for good. It is
	// therefore replaced by the pixels of this frame instead of masking them.
	const bool stale_dimmed_copy =
	        foreign_total * 3 >= Kao64Width * Kao64Height &&
	        changed_bits * 50 <= KaoRelationBits;
	if (stale_dimmed_copy) {
		capture_box_pixels(region, stride, command);
		command.covered.fill(0);
		command.overlay_like = false;
		return 0;
	}
	// A stored picture that no longer matches the box at all (it was taken while
	// the screen was still dark) says nothing here, so it is ignored instead of
	// masking the whole box. Such a stale reference differs in *every* pixel
	// while the equality pattern it was built from is still practically intact;
	// a panel, a window or another picture breaks the pattern as well. The
	// "yes/no" box covers only part of the portrait and leaves the pattern
	// around it in place, so it must not be mistaken for a stale reference.
	const bool reference_unusable =
	        foreign_total * 20 >= Kao64Width * Kao64Height * 19 &&
	        changed_bits * 2 <= KaoRelationBits;
	if (foreign_total > 0 && !reference_unusable) {
		for (int index = 0; index < Kao64Width * Kao64Height; ++index) {
			if (foreign[index] != 0) {
				command.covered[index] = 1;
			}
		}
	}
	// A panel painted over the box is recognised by its own shape, so it stays
	// on top of the artwork even where its blank paper happens to carry the very
	// colour of the picture underneath.
	mark_panel_pixels(region, stride, command.x, command);
	mark_flat_panel_pixels(region, stride, command);

	int covered = 0;
	for (int row = 0; row < Kao64Height; ++row) {
		auto* marks = command.covered.data() +
		              static_cast<size_t>(row) * Kao64Width;
		const auto* line = region + static_cast<size_t>(row) * stride;
		// A broken pair only outlines what was painted on top. The inside of a
		// panel, on the other hand, carries no outline of its own because the
		// picture under it can match there. A stretch of one single colour is
		// therefore handed over as well: the artwork looks exactly the same
		// there, so nothing is lost, while the inside of a dialog or a window is
		// covered instead of being painted over.
		int gap_start = -1;
		bool gap_flat = true;
		for (int col = 0; col <= Kao64Width; ++col) {
			if (col < Kao64Width && marks[col] == 0) {
				if (gap_start < 0) {
					gap_start = col;
					gap_flat  = true;
				} else if (line[col] != line[gap_start]) {
					gap_flat = false;
				}
				continue;
			}
			if (gap_start >= 0) {
				// Only a stretch between two marked pixels belongs to something
				// painted on top. A flat run that reaches the border of the box is
				// just a flat part of the picture and must not be masked.
				if (gap_flat && gap_start > 0 && col < Kao64Width) {
					for (int fill = gap_start; fill < col; ++fill) {
						marks[fill] = 1;
					}
				}
				gap_start = -1;
			}
		}
		for (int col = 0; col < Kao64Width; ++col) {
			covered += marks[col];
		}
	}
	command.overlay_like = covered * 2 > Kao64Width * Kao64Height;
	return covered;
}

// Temporary: write the current frame buffer with the tested box outlined, so
// the actual contents of a rejected box can be inspected after a test run.
void diag_dump_source(const uint32_t* source, const int width, const int height,
                      const char* tag, const DrawCommand& command)
{
	if (diag_dump_count >= 40) {
		return;
	}
	++diag_dump_count;

	char name[96] = {};
	std::snprintf(name,
	              sizeof(name),
	              "sea2-diag-%02d-%s-e%03u-%u-%u.png",
	              diag_dump_count,
	              tag,
	              command.entry,
	              command.x,
	              command.y);

	auto* surface = SDL_CreateRGBSurfaceWithFormat(0,
	                                              width,
	                                              height,
	                                              32,
	                                              SDL_PIXELFORMAT_RGBA32);
	if (!surface) {
		return;
	}
	if (SDL_LockSurface(surface) != 0) {
		SDL_FreeSurface(surface);
		return;
	}

	for (int y = 0; y < height; ++y) {
		auto* row = static_cast<uint8_t*>(surface->pixels) +
		            static_cast<size_t>(y) * surface->pitch;
		const auto* in = source + static_cast<size_t>(y) * width;
		for (int x = 0; x < width; ++x) {
			const auto value   = in[x];
			row[x * 4 + 0]     = static_cast<uint8_t>((value >> 16) & 0xff);
			row[x * 4 + 1]     = static_cast<uint8_t>((value >> 8) & 0xff);
			row[x * 4 + 2]     = static_cast<uint8_t>(value & 0xff);
			row[x * 4 + 3]     = 0xff;
		}
	}

	const int box_x = command.x;
	const int box_y = command.y + ContentYOffset;
	for (int dy = 0; dy < Kao64Height; ++dy) {
		for (int dx = 0; dx < Kao64Width; ++dx) {
			if (dx > 1 && dx < Kao64Width - 2 && dy > 1 &&
			    dy < Kao64Height - 2) {
				continue;
			}
			const int px = box_x + dx;
			const int py = box_y + dy;
			if (px < 0 || py < 0 || px >= width || py >= height) {
				continue;
			}
			auto* pixel = static_cast<uint8_t*>(surface->pixels) +
			              static_cast<size_t>(py) * surface->pitch + px * 4;
			pixel[0] = 255;
			pixel[1] = 0;
			pixel[2] = 0;
			pixel[3] = 255;
		}
	}

	SDL_UnlockSurface(surface);
	IMG_SavePNG(surface, name);
	SDL_FreeSurface(surface);
}

// Temporary: write one box four times enlarged - as the game drew it, as it was
// stored when the portrait was frozen, and with the pixels that the mask hands
// back to the game painted red - so a box that is still painted over can be
// compared by hand.
void diag_dump_box(const uint32_t* pixels, const int stride, const uint8_t* mask,
                   const char* tag, const DrawCommand& command)
{
	if (diag_box_dump_count >= 24) {
		return;
	}
	++diag_box_dump_count;

	constexpr int Scale = 4;
	const int width     = Kao64Width * Scale;
	const int height    = Kao64Height * Scale;

	char name[96] = {};
	std::snprintf(name,
	              sizeof(name),
	              "sea2-box-%02d-%s-e%03u-%u-%u.png",
	              diag_box_dump_count,
	              tag,
	              command.entry,
	              command.x,
	              command.y);

	auto* surface = SDL_CreateRGBSurfaceWithFormat(0,
	                                              width,
	                                              height,
	                                              32,
	                                              SDL_PIXELFORMAT_RGBA32);
	if (!surface) {
		return;
	}
	if (SDL_LockSurface(surface) != 0) {
		SDL_FreeSurface(surface);
		return;
	}

	for (int y = 0; y < height; ++y) {
		auto* row      = static_cast<uint8_t*>(surface->pixels) +
		            static_cast<size_t>(y) * surface->pitch;
		const auto* in = pixels + static_cast<size_t>(y / Scale) * stride;
		const auto* mk = mask ? mask + static_cast<size_t>(y / Scale) * Kao64Width
		                      : nullptr;
		for (int x = 0; x < width; ++x) {
			const auto value = in[x / Scale];
			auto r = static_cast<uint8_t>((value >> 16) & 0xff);
			auto g = static_cast<uint8_t>((value >> 8) & 0xff);
			auto b = static_cast<uint8_t>(value & 0xff);
			if (mk && mk[x / Scale] != 0) {
				r = 255;
				g = 0;
				b = 0;
			}
			row[x * 4 + 0] = r;
			row[x * 4 + 1] = g;
			row[x * 4 + 2] = b;
			row[x * 4 + 3] = 0xff;
		}
	}

	SDL_UnlockSurface(surface);
	IMG_SavePNG(surface, name);
	SDL_FreeSurface(surface);
}

// Writes the finished picture around one box with the blocks that were handed
// back to the game tinted green, so the torn spots can be compared with the mask
// that was supposed to protect them.
void diag_dump_composite(const uint32_t* destination, const int stride,
                         const DrawCommand& command)
{
	if (diag_hd_dump_count >= 60) {
		return;
	}
	++diag_hd_dump_count;

	constexpr int Margin = 16;
	const int box_left   = command.x * HdScale;
	const int box_top    = (command.y + ContentYOffset) * HdScale;
	const int left       = std::max(0, box_left - Margin * HdScale);
	const int top        = std::max(0, box_top - Margin * HdScale);
	const int width = std::min(VgaWidth * HdScale - left,
	                           (Kao64Width + 2 * Margin) * HdScale);
	const int height = std::min(VgaHeight * HdScale - top,
	                            (Kao64Height + 2 * Margin) * HdScale);
	if (width <= 0 || height <= 0) {
		return;
	}

	char name[96] = {};
	std::snprintf(name,
	              sizeof(name),
	              "sea2-hd-%02d-e%03u-%u-%u.png",
	              diag_hd_dump_count,
	              command.entry,
	              command.x,
	              command.y);

	auto* surface = SDL_CreateRGBSurfaceWithFormat(0,
	                                              width,
	                                              height,
	                                              32,
	                                              SDL_PIXELFORMAT_RGBA32);
	if (!surface) {
		return;
	}
	if (SDL_LockSurface(surface) != 0) {
		SDL_FreeSurface(surface);
		return;
	}

	for (int y = 0; y < height; ++y) {
		auto* out = static_cast<uint8_t*>(surface->pixels) +
		            static_cast<size_t>(y) * surface->pitch;
		const auto* in = destination +
		                 static_cast<size_t>(top + y) * stride + left;
		const int src_y = (top + y) / HdScale - (command.y + ContentYOffset);
		for (int x = 0; x < width; ++x) {
			const auto value = in[x];
			int r = static_cast<int>((value >> 16) & 0xff);
			int g = static_cast<int>((value >> 8) & 0xff);
			int b = static_cast<int>(value & 0xff);
			const int src_x = (left + x) / HdScale - command.x;
			if (src_x >= 0 && src_x < Kao64Width && src_y >= 0 &&
			    src_y < Kao64Height &&
			    command.covered[static_cast<size_t>(src_y) * Kao64Width +
			                    static_cast<size_t>(src_x)] != 0) {
				r = r * 3 / 5;
				g = g * 3 / 5 + 100;
				b = b * 3 / 5;
			}
			out[x * 4 + 0] = static_cast<uint8_t>(r);
			out[x * 4 + 1] = static_cast<uint8_t>(g);
			out[x * 4 + 2] = static_cast<uint8_t>(b);
			out[x * 4 + 3] = 0xff;
		}
	}

	SDL_UnlockSurface(surface);
	IMG_SavePNG(surface, name);
	SDL_FreeSurface(surface);
}

// Same rule as the text cells: nothing about the portrait is decided once and
// remembered forever. Every composited frame re-reads the box and says whether
// the HQX portrait may be drawn over it *this frame*. A box that shows other
// artwork is skipped, never deleted, so a later redraw of the same portrait
// brings it back on its own.
//
// This is the algorithm for every portrait box of the game itself: the
// character window, a dialog, a menu. The six fixed boxes of the character
// selection screen are handled by `scene_box_shows_portrait` instead, and this
// function never looks at that screen's state, so the two algorithms cannot
// influence each other.
bool dialog_box_shows_portrait(DrawCommand& command, const uint32_t* source,
                               const int source_width, const int source_height)
{
	if (source_width != VgaWidth || source_height != VgaHeight) {
		return false;
	}

	const auto* region = kao_box_origin(command, source, source_width);

	std::array<uint8_t, KaoRelationBytes> current = {};
	build_relation_bits(region, source_width, current);

	// The reference has to be the finished picture, never a frame in the middle
	// of the window being drawn. So the box is watched until its pattern
	// repeats, and only then frozen.
	if (!command.locked) {
		const bool unchanged = command.stable > 0 && current == command.reference;
		if (unchanged) {
			if (command.stable < 0xff) {
				++command.stable;
			}
			// A window, a dialog or a half drawn portrait differs from the frame
			// before it, so the pattern is simply re-settled until the picture
			// really repeats.
			if (command.stable >= KaoLockStableFrames &&
			    count_relation_structure(current) >= KaoMinRelationStructure) {
				command.locked = true;
				capture_box_pixels(region, source_width, command);
				diag("LOCK entry=%u x=%u y=%u structure=%d unsettled=%u frame=%u",
				     command.entry,
				     command.x,
				     command.y,
				     count_relation_structure(current),
				     command.unsettled,
				     composite_count);
				return true;
			}
		} else {
			command.reference = current;
			command.stable    = 1;
		}

		// The portrait of a window is already complete in the frame its command
		// appears, so drawing it only after the pattern repeated leaves the low
		// resolution face on screen for a few frames - the flash that can be seen
		// when a dialog is opened for the first time. It is drawn at once instead,
		// but only while it plainly is a finished portrait and nothing can be
		// sitting on top of it: the screen is not fading and the box carries the
		// structure of a face instead of a flat panel. The box is not frozen here,
		// so a transient frame can never become
		// the reference; the locking rule above still decides that.
		const auto structure = count_relation_structure(current);
		if (!screen_fading && structure >= KaoDialogPreviewMinStructure) {
			command.covered.fill(0);
			command.matched_last = true;
			diag("PREVIEW entry=%u x=%u y=%u structure=%d frame=%u",
			     command.entry,
			     command.x,
			     command.y,
			     structure,
			     composite_count);
			return true;
		}

		if (command.unsettled < KaoSettleGiveUpFrames) {
			++command.unsettled;
			return false;
		}
		// Never repeats itself: fall back to drawing it unconditionally.
		if (!command.reported) {
			command.reported = true;
			diag("GIVEUP entry=%u x=%u y=%u structure=%d",
			     command.entry,
			     command.x,
			     command.y,
			     count_relation_structure(current));
		}
		return true;
	}

	const auto change = compare_relation_bits(command.reference, current);
	const int reference_structure = count_relation_structure(command.reference);
	const int current_structure   = count_relation_structure(current);

	// A window, a label or the pointer may be drawn over the picture. Those
	// pixels are already flagged, and drawing the portrait without them keeps
	// the box in high resolution instead of dropping it for the whole scene.
	int foreign_pixels  = mark_foreign_pixels(region, source_width, current, command);

	int matching_pixels = (Kao64Width * Kao64Height) - foreign_pixels;

	// A window, a dialog and the pointer are solid blocks, so their pixels form
	// long runs inside a row. Pixels that disagree here and there while the
	// pattern of the picture itself stays the same are brightness noise instead:
	// a dithered fade or the very same artwork redrawn. Cutting those out would
	// leave the low resolution face showing through the high resolution one, so
	// the portrait is drawn whole and the stored pixels are taken anew.
	const bool structure_similar =
	        std::abs(current_structure - reference_structure) * 10 <=
	        reference_structure;
	if (!screen_fading && !command.overlay_like &&
	    change.total() <= KaoRelationTolerance && structure_similar &&
	    matching_pixels < KaoMinCleanMatchingPixels) {
		capture_box_pixels(region, source_width, command);
		command.covered.fill(0);
		foreign_pixels  = 0;
		matching_pixels = Kao64Width * Kao64Height;
		diag("REFRESH entry=%u x=%u y=%u frame=%u diff=%d structure=%d ref=%d",
		     command.entry,
		     command.x,
		     command.y,
		     composite_count,
		     change.total(),
		     current_structure,
		     reference_structure);
	}

	const int min_matching = command.overlay_like ? KaoMinOverlayMatchingPixels
	                                              : KaoMinCleanMatchingPixels;

	// While the whole screen fades out, the box fades with it. The stored pixels
	// are fitted to the current frame, so a fit that darkens them proves the
	// picture is still there, only dimmed. Drawing it with the very same gain
	// keeps the portrait in step with the rest of the screen instead of leaving
	// it lit and then making it pop out.
	bool box_dimming = false;
	for (auto channel = 0; channel < 3; ++channel) {
		if (command.fade_gain[channel] < 0.995f) {
			box_dimming = true;
		}
	}
	const bool fade_drawing = screen_fading && box_dimming &&
	                          change.different_to_equal == 0 &&
	                          matching_pixels >= KaoMinFittedMatchingPixels;
	if (fade_drawing) {
		command.matched_last = true;
		return true;
	}

	if (change.total() > KaoRelationTolerance) {
		// The fade finished while the box was locked: the picture now holds more
		// detail than the dim frame that was frozen. Following it keeps the
		// portrait on screen instead of dropping it.
		//
		// A fade brightens the box pixel by pixel, so two neighbouring pixels
		// that differed keep differing and further pairs start to differ: the
		// picture only gains detail. `equal_to_different` counts the pairs that
		// went the other way, so a zero there proves nothing was painted over
		// the picture. The pixel comparison cannot be trusted meanwhile (the
		// dim frame is not a linear scale of the finished one), so this is the
		// one case where the stored pixels are followed without asking them.
		// The whole screen has to be fading for that: a dialog adds detail too.
		const bool fade_finishing = screen_fading &&
		                            change.equal_to_different == 0;

		if (current_structure > reference_structure &&
		    (fade_finishing || foreign_pixels <= KaoMaxForeignPixels) &&
		    command.updates < KaoMaxReferenceUpdates) {
			++command.updates;
			command.matched_last = true;
			command.masked_last  = false;
			command.reference    = current;
			capture_box_pixels(region, source_width, command);
			if (fade_finishing && foreign_pixels > 0) {
				// The old pixels were too dim to tell a window from the fade, so
				// the flags they produced are not trustworthy. The detail test
				// already proved the picture is intact, draw all of it.
				command.covered.fill(0);
			}
			diag("HEAL entry=%u x=%u y=%u frame=%u structure=%d was=%d eq2ne=%d ne2eq=%d",
			     command.entry,
			     command.x,
			     command.y,
			     composite_count,
			     current_structure,
			     reference_structure,
			     change.equal_to_different,
			     change.different_to_equal);
			return true;
		}
		// The picture is no longer there. Flat areas of another screen, a dimmed
		// scene or a cleared box coincide with flat areas of the picture by the
		// thousand, so the pattern has to stay about as rich as the stored one
		// before the pixels are trusted at all.
		const auto min_structure = std::max(reference_structure / 2,
		                                    KaoMinRelationStructure);
		if (current_structure < min_structure ||
		    matching_pixels < min_matching) {
			if (command.matched_last) {
				command.matched_last = false;
				diag("MISS entry=%u x=%u y=%u frame=%u diff=%d eq2ne=%d ne2eq=%d structure=%d ref=%d matching=%d tolerance=%d",
				     command.entry,
				     command.x,
				     command.y,
				     composite_count,
				     change.total(),
				     change.equal_to_different,
				     change.different_to_equal,
				     current_structure,
				     reference_structure,
				     matching_pixels,
				     KaoRelationTolerance);
			}
			return false;
		}
	} else if (matching_pixels < min_matching) {
		// The pattern still looks like the picture, yet the stored pixels no
		// longer match it. That is what a reference taken during a fade looks
		// like: the geometry is identical, only the colours are not. Such a box
		// takes a fresh reference instead of dropping the portrait for the whole
		// scene. Anything drawn over the box changes the pattern far more, so a
		// dialog or a label can never trigger this.
		if (!screen_fading && change.total() <= KaoStaleReferenceTolerance &&
		    command.relocks < KaoMaxRelocks) {
			++command.relocks;
			command.locked       = false;
			command.stable       = 0;
			command.unsettled    = 0;
			command.updates      = 0;
			command.pixels_valid = false;
			command.covered.fill(0);
			command.reference = current;
			diag("RELOCK entry=%u x=%u y=%u frame=%u diff=%d matching=%d foreign=%d",
			     command.entry,
			     command.x,
			     command.y,
			     composite_count,
			     change.total(),
			     matching_pixels,
			     foreign_pixels);
			return false;
		}
		// The pattern still looks like the picture, yet most of it is gone: the
		// box cannot be trusted, and drawing would leave artwork behind.
		if (command.matched_last) {
			command.matched_last = false;
			diag("MISS entry=%u x=%u y=%u frame=%u diff=%d matching=%d foreign=%d",
			     command.entry,
			     command.x,
			     command.y,
			     composite_count,
			     change.total(),
			     matching_pixels,
			     foreign_pixels);
		}
		return false;
	}

	// The stored pixels are only refreshed from a box that repeats the stored
	// pattern exactly and holds nothing but the picture, never from one that is
	// still changing.
	if (change.total() == 0 && foreign_pixels == 0) {
		capture_box_pixels(region, source_width, command);
	}

	if (foreign_pixels == 0) {
		if (command.masked_last) {
			command.masked_last = false;
			diag("UNMASKED entry=%u x=%u y=%u frame=%u diff=%d",
			     command.entry,
			     command.x,
			     command.y,
			     composite_count,
			     change.total());
		}
	} else if (!command.masked_last) {
		command.masked_last = true;
		diag("MASKED entry=%u x=%u y=%u frame=%u foreign=%d diff=%d",
		     command.entry,
		     command.x,
		     command.y,
		     composite_count,
		     foreign_pixels,
		     change.total());
	}

	if (!command.matched_last) {
		command.matched_last = true;
		diag("MATCH entry=%u x=%u y=%u frame=%u diff=%d foreign=%d",
		     command.entry,
		     command.x,
		     command.y,
		     composite_count,
		     change.total(),
		     foreign_pixels);
	}
	return true;
}

// The character selection screen is the only screen that shows six portraits at
// once, always at the same places. That makes a rule of its own possible: a box
// is drawn as long as the *screen* is recognised, not as long as the box alone
// is intact, and only what was really painted over the picture is masked out.
//
// This function is the whole algorithm for those six boxes. It takes every
// decision on its own and shares no flag, threshold or state with
// `dialog_box_shows_portrait`, so the two algorithms can be changed without the
// one ever affecting the other.
bool scene_box_shows_portrait(DrawCommand& command, const uint32_t* source,
                              const int source_width, const int source_height)
{
	if (source_width != VgaWidth || source_height != VgaHeight) {
		return false;
	}

	const auto* region = kao_box_origin(command, source, source_width);

	std::array<uint8_t, KaoRelationBytes> current = {};
	build_relation_bits(region, source_width, current);

	// The box has to hold the finished picture before it may be frozen.
	if (!command.locked) {
		const bool unchanged = command.stable > 0 && current == command.reference;
		if (unchanged) {
			if (command.stable < 0xff) {
				++command.stable;
			}
			if (command.stable >= KaoLockStableFrames &&
			    count_relation_structure(current) >= KaoMinRelationStructure) {
				command.locked = true;
				capture_box_pixels(region, source_width, command);
				diag("SCENE LOCK entry=%u x=%u y=%u structure=%d unsettled=%u frame=%u",
				     command.entry,
				     command.x,
				     command.y,
				     count_relation_structure(current),
				     command.unsettled,
				     composite_count);
				return true;
			}
		} else {
			command.reference = current;
			command.stable    = 1;
		}

		// A box that never repeats itself can not be verified frame by frame. It
		// is given up on after a while and drawn unconditionally again.
		if (command.unsettled < KaoSettleGiveUpFrames) {
			++command.unsettled;
			return false;
		}
		// Never repeats itself: fall back to drawing it unconditionally.
		if (!command.reported) {
			command.reported = true;
			diag("SCENE GIVEUP entry=%u x=%u y=%u structure=%d",
			     command.entry,
			     command.x,
			     command.y,
			     count_relation_structure(current));
		}
		return true;
	}

	// The screen is not recognised as the character selection screen any more.
	// The box is then followed by the generic rule, which is what it did before
	// the scene was recognised and what keeps a portrait from being left behind
	// on the next screen.
	if (!portrait_scene_active) {
		return dialog_box_shows_portrait(command, source, source_width,
		                                 source_height);
	}

	const auto change = compare_relation_bits(command.reference, current);
	const int reference_structure = count_relation_structure(command.reference);
	const int current_structure   = count_relation_structure(current);

	// A box can be locked half way through the fade-in, and then the frozen
	// pattern holds far less detail than the finished portrait. Measured
	// against such a pattern the whole box counts as changed and would never
	// be drawn again. A fade adds detail - a handful of pairs can also be lost
	// to the dithering of the dimmed screen -, while a dialog, a label or a
	// window flattens the picture under it and loses far more. So a richer
	// pattern that kept almost all of its detail is the finished picture, and
	// it replaces the old one instead of hiding the box.
	const bool detail_gained =
	        !screen_fading &&
	        change.total() > KaoRelationTolerance &&
	        change.equal_to_different * 10 <= change.total() &&
	        current_structure > reference_structure &&
	        command.updates < KaoMaxReferenceUpdates;
	if (detail_gained) {
		++command.updates;
		command.reference = current;
		capture_box_pixels(region, source_width, command);
		command.covered.fill(0);
		if (composite_count % 60 == 0) {
			diag("SCENE HEAL entry=%u x=%u y=%u frame=%u structure=%d was=%d ne2eq=%d",
			     command.entry, command.x, command.y, composite_count,
			     current_structure, reference_structure,
			     change.different_to_equal);
		}
		command.matched_last = true;
		return true;
	}

	// The reference can be taken while the game is still fading its screen in.
	// The pattern survives such a fade, but the stored colours stay dark, and
	// then every pixel of the box looks like something painted on top. As soon
	// as the box repeats the stored pattern and nothing is drawn over it, the
	// very pixels are taken once more.
	if (!screen_fading && command.pixels_valid && change.total() == 0) {
		capture_box_pixels(region, source_width, command);
	}

	// What is left to protect is only what was really painted over the picture.
	// It is measured on the pattern, not on the colours: brightness, dithering
	// and a redraw leave the pattern of the portrait intact, so the portrait is
	// drawn whole even though its pixels look different from the stored ones,
	// while a dialog or the pointer breaks the pattern exactly where it sits.
	int covered_pixels = 0;
	if (screen_fading) {
		// The whole screen is rescaled. The pixels are still the picture, so
		// nothing is masked; the fit only keeps the artwork dimming with the
		// rest of the screen instead of staying lit.
		mark_foreign_pixels(region, source_width, current, command);
		command.covered.fill(0);
	} else {
		covered_pixels = mark_changed_pixels(region, source_width,
		                                     command.reference, current,
		                                     command);
	}

	if (composite_count % 120 == 0) {
		diag("SCENE entry=%u x=%u y=%u frame=%u covered=%d diff=%d lost=%d gained=%d overlay=%d fading=%d gain=%.2f/%.2f/%.2f off=%.0f/%.0f/%.0f",
		     command.entry, command.x, command.y, composite_count,
		     covered_pixels, change.total(), change.equal_to_different,
		     change.different_to_equal, command.overlay_like ? 1 : 0,
		     screen_fading ? 1 : 0, command.fade_gain[0],
		     command.fade_gain[1], command.fade_gain[2],
		     command.fade_offset[0], command.fade_offset[1],
		     command.fade_offset[2]);
	}
	command.matched_last = true;
	return true;
}

uint8_t blend_channel(const uint8_t src, const uint8_t dst, const uint8_t alpha)
{
	return static_cast<uint8_t>((src * alpha + dst * (255 - alpha) + 127) / 255);
}

const GlyphAtlas* get_glyph_atlas()
{
	if (glyph_atlas.state == AssetState::Loaded) {
		return &glyph_atlas;
	}
	if (glyph_atlas.state == AssetState::Missing) {
		return nullptr;
	}
	glyph_atlas.state = AssetState::Missing;

	const auto path        = assets_root / "simplified_atlas.png";
	const auto path_string = path.string();

	auto* loaded = IMG_Load(path_string.c_str());
	if (!loaded) {
		LOG_WARNING("SEA2HD: Cannot load simplified atlas: %s", IMG_GetError());
		return nullptr;
	}

	auto* converted = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
	SDL_FreeSurface(loaded);
	if (!converted) {
		LOG_WARNING("SEA2HD: Cannot decode simplified atlas: %s", SDL_GetError());
		return nullptr;
	}

	const auto rows =
	        (sea2_glyph_map::RecordCount + GlyphAtlasColumns - 1) / GlyphAtlasColumns;
	if (converted->w != GlyphAtlasColumns * GlyphTileWidth ||
	    converted->h != rows * GlyphTileHeight) {
		LOG_WARNING("SEA2HD: Simplified atlas has invalid dimensions %dx%d",
		            converted->w, converted->h);
		SDL_FreeSurface(converted);
		return nullptr;
	}

	if (SDL_LockSurface(converted) != 0) {
		LOG_WARNING("SEA2HD: Cannot lock simplified atlas: %s", SDL_GetError());
		SDL_FreeSurface(converted);
		return nullptr;
	}

	glyph_atlas.width  = converted->w;
	glyph_atlas.height = converted->h;
	glyph_atlas.alpha.resize(
	        static_cast<size_t>(glyph_atlas.width) * glyph_atlas.height);
	for (int y = 0; y < glyph_atlas.height; ++y) {
		const auto* src = static_cast<const uint8_t*>(converted->pixels) +
		                  static_cast<size_t>(y) * converted->pitch;
		auto* dst = glyph_atlas.alpha.data() +
		            static_cast<size_t>(y) * glyph_atlas.width;
		for (int x = 0; x < glyph_atlas.width; ++x) {
			dst[x] = src[x * 4 + 0];
		}
	}
	SDL_UnlockSurface(converted);
	SDL_FreeSurface(converted);

	glyph_atlas.available.assign(sea2_glyph_map::RecordCount, 0);
	for (size_t tile = 0; tile < sea2_glyph_map::RecordCount; ++tile) {
		const auto tx = static_cast<int>(tile % GlyphAtlasColumns) * GlyphTileWidth;
		const auto ty = static_cast<int>(tile / GlyphAtlasColumns) * GlyphTileHeight;
		uint32_t ink = 0;
		for (int y = 0; y < GlyphTileHeight && ink < 32; ++y) {
			const auto* row = glyph_atlas.alpha.data() +
			                  static_cast<size_t>(ty + y) * glyph_atlas.width + tx;
			for (int x = 0; x < GlyphTileWidth; ++x) {
				ink += row[x];
			}
		}
		glyph_atlas.available[tile] = ink >= 32;
	}

	glyph_atlas.state = AssetState::Loaded;
	LOG_MSG("SEA2HD: Loaded simplified atlas %dx%d", glyph_atlas.width,
	        glyph_atlas.height);
	return &glyph_atlas;
}

bool is_blank_glyph_bitmap(const uint8_t* bytes)
{
	for (int i = 0; i < TextGlyphBytes; ++i) {
		if (bytes[i] != 0) {
			return false;
		}
	}
	return true;
}

void index_glyph_bitmap(const uint16_t record, const uint8_t* bytes)
{
	if (record == 0) {
		return;
	}
	// A blank cell carries no identity; indexing it would make every empty
	// cell resolve to the same record.
	if (is_blank_glyph_bitmap(bytes)) {
		return;
	}

	GlyphMask mask = {};
	std::memcpy(mask.data(), bytes, TextGlyphBytes);
	record_masks[record] = mask;
	const auto result = glyph_index.emplace(mask, record);
	if (result.second) {
		++glyph_index_used;
	} else {
		// Same bitmap seen again; keep the newest record, which is the one
		// the renderer just loaded.
		result.first->second = record;
	}
}

uint16_t lookup_glyph_bitmap(const uint8_t* bytes)
{
	GlyphMask mask = {};
	std::memcpy(mask.data(), bytes, TextGlyphBytes);
	const auto it = glyph_index.find(mask);
	return it == glyph_index.end() ? 0 : it->second;
}

void reset_glyph_index()
{
	glyph_index.clear();
	record_masks.clear();
	glyph_index_used  = 0;
	glyph_match_total = 0;
	glyph_miss_total  = 0;
}

// Registers one text cell. The glyph the renderer blitted here is remembered
// as well: a cell that the pointer covers keeps its simplified character from
// that piece of information alone.
void register_text_cell(const uint16_t x, const uint16_t y,
                        const uint16_t record, const GlyphMask& mask)
{
	if (x + TextGlyphWidth > VgaWidth) {
		return;
	}
	if (y + TextGlyphHeight > VgaHeight - ContentYOffset) {
		return;
	}
	for (auto& cell : text_cells) {
		if (cell.x == x && cell.y == y) {
			if (record != 0) {
				cell.known  = true;
				cell.record = record;
				cell.mask   = mask;
			}
			return;
		}
	}
	if (text_cells.size() >= MaxTextCells) {
		text_cells.pop_front();
	}
	TextCell cell = {};
	cell.x        = x;
	cell.y        = y;
	if (record != 0) {
		cell.known  = true;
		cell.record = record;
		cell.mask   = mask;
	}
	text_cells.push_back(cell);
	++glyph_match_total;
	frame_dirty = true;
}

// Temporary diagnostics: a registered cell that cannot be identified is written
// to the log once per distinct appearance, but only while the pointer sits on
// it, so the highlight the game paints over hovered text can be read back.
uint32_t diag_text_lines = 0;
std::unordered_map<uint32_t, uint32_t> diag_text_signatures = {};

bool pointer_over_text_cell(const uint16_t x, const uint16_t y,
                            const int Margin = 8)
{
	uint16_t pointer_x = 0;
	uint16_t pointer_y = 0;
	if (!MOUSEDOS_GetCursorPosition(pointer_x, pointer_y)) {
		return false;
	}
	const int px = static_cast<int>(pointer_x);
	const int py = static_cast<int>(pointer_y);
	return px >= static_cast<int>(x) - Margin &&
	       px < static_cast<int>(x) + TextGlyphWidth + Margin &&
	       py >= static_cast<int>(y) + ContentYOffset - Margin &&
	       py < static_cast<int>(y) + ContentYOffset + TextGlyphHeight + Margin;
}

void diag_text_failure(const uint16_t x, const uint16_t y, const int reason,
                       const int used, const uint32_t* colors,
                       const int* counts, const int* matched,
                       const uint8_t* color_of)
{
	if (diag_text_lines >= 150) {
		return;
	}
	if (!pointer_over_text_cell(x, y)) {
		return;
	}
	uint32_t signature = 2166136261u ^ static_cast<uint32_t>(reason);
	for (int i = 0; i < used; ++i) {
		signature = (signature ^ colors[i]) * 16777619u;
		signature = (signature ^ static_cast<uint32_t>(counts[i])) * 16777619u;
	}
	const uint32_t key = (static_cast<uint32_t>(x) << 16) | y;
	const auto it = diag_text_signatures.find(key);
	if (it != diag_text_signatures.end() && it->second == signature) {
		return;
	}
	diag_text_signatures[key] = signature;

	diag("TXTFAIL x=%u y=%u reason=%d used=%d", x, y, reason, used);
	for (int i = 0; i < used; ++i) {
		diag("  TXTCOLOR c=%d rgb=%06x count=%d match=%d",
		     i, colors[i] & 0xffffffu, counts[i], matched[i]);
	}
	for (int row = 0; row < TextGlyphHeight; ++row) {
		char line[TextGlyphWidth + 1] = {};
		for (int col = 0; col < TextGlyphWidth; ++col) {
			line[col] = static_cast<char>('0' + color_of[row * TextGlyphWidth + col]);
		}
		diag("  TXTROW %s", line);
	}
	diag_text_lines += 2 + used + TextGlyphHeight;
}

// Re-derives the glyph from the pixels currently on screen. Every distinct
// colour of the cell is tried as the ink; a candidate is accepted only when
// the resulting 224-bit mask indexes exactly one record. Requiring a unique
// record is what keeps portraits, icons and dither patterns from being
// mistaken for text, and it makes the highlight (XOR) colours harmless.
bool identify_text_cell(const uint32_t* source, const int source_width,
                        const uint16_t x, const uint16_t y, uint16_t& record,
                        uint32_t& background, uint32_t& ink,
                        GlyphMask* cell_mask = nullptr)
{
	constexpr int CellPixels = TextGlyphWidth * TextGlyphHeight;

	uint32_t colors[CellPixels]  = {};
	int counts[CellPixels]       = {};
	uint8_t color_of[CellPixels] = {};
	int used                     = 0;

	const auto* cell = source +
	                   static_cast<size_t>(y + ContentYOffset) * source_width + x;
	int position = 0;
	for (int row = 0; row < TextGlyphHeight; ++row) {
		const auto* line = cell + static_cast<size_t>(row) * source_width;
		for (int col = 0; col < TextGlyphWidth; ++col, ++position) {
			const auto color = line[col];
			int i = 0;
			for (; i < used; ++i) {
				if (colors[i] == color) {
					break;
				}
			}
			if (i == used) {
				// Ink, the dithered background and the highlight bar are the
				// most a text cell can hold; anything richer is not a glyph
				// cell.
				if (used == MaxTextCellColors) {
					int matched[MaxTextCellColors] = {};
					for (int k = 0; k < used; ++k) {
						matched[k] = -1;
					}
					diag_text_failure(x, y, 1, used, colors, counts, matched,
					                  color_of);
					return false;
				}
				colors[used] = color;
				counts[used] = 0;
				++used;
			}
			++counts[i];
			color_of[position] = static_cast<uint8_t>(i);
		}
	}

	int matched[MaxTextCellColors] = {};
	for (int i = 0; i < MaxTextCellColors; ++i) {
		matched[i] = -1;
	}
	if (used < 2) {
		diag_text_failure(x, y, 2, used, colors, counts, matched, color_of);
		return false;
	}

	int matches = 0;
	for (int candidate = 0; candidate < used; ++candidate) {
		matched[candidate] = 0;
		if (counts[candidate] < MinGlyphInkPixels ||
		    counts[candidate] > MaxGlyphInkPixels) {
			continue;
		}
		GlyphMask mask = {};
		for (int i = 0; i < CellPixels; ++i) {
			if (color_of[i] == candidate) {
				mask[i >> 3] |= static_cast<uint8_t>(0x80u >> (i & 7));
			}
		}
		const auto it = glyph_index.find(mask);
		if (it == glyph_index.end()) {
			continue;
		}
		matched[candidate] = 1;
		if (++matches > 1) {
			matched[candidate] = 2;
			diag_text_failure(x, y, 4, used, colors, counts, matched,
			                  color_of);
			return false;
		}
		record = it->second;
		if (cell_mask) {
			*cell_mask = mask;
		}
		ink    = colors[candidate];
		int best = -1;
		for (int i = 0; i < used; ++i) {
			if (i != candidate && (best < 0 || counts[i] > counts[best])) {
				best = i;
			}
		}
		background = best >= 0 ? colors[best] : colors[candidate];
	}
	if (matches != 1) {
		diag_text_failure(x, y, 3, used, colors, counts, matched, color_of);
		return false;
	}
	return true;
}

// The mouse pointer is drawn on top of the text, and it replaces the pixels it
// covers with its own colours. The pattern then no longer indexes a record and
// the cell would keep the game's traditional character. The cell knows which
// glyph the renderer blitted there, so the pixels are checked against that very
// shape instead: the ink is the colour that fills the glyph, the background the
// colour that fills the rest.
//
// The pointer is a thin outline sprite, so it only paints a few dozen extra
// ink coloured pixels over the paper and hides almost none of the glyph. Those
// pixels are allowed to sit on the background - they are copied back on top of
// the tile afterwards - while the glyph itself still has to be followed
// closely, which is what keeps a different character, a panel or another scene
// from being replaced.
bool identify_text_cell_while_covered(const uint32_t* source,
                                      const int source_width, const uint16_t x,
                                      const uint16_t y, const GlyphMask& mask,
                                      uint32_t& background, uint32_t& ink)
{
	constexpr int CellPixels = TextGlyphWidth * TextGlyphHeight;

	int mask_pixels = 0;
	for (int i = 0; i < CellPixels; ++i) {
		if (mask[i / 8] & (0x80u >> (i % 8))) {
			++mask_pixels;
		}
	}
	if (mask_pixels < MinGlyphInkPixels || mask_pixels > MaxGlyphInkPixels) {
		return false;
	}
	const int free_pixels = CellPixels - mask_pixels;

	uint32_t colors[CellPixels]  = {};
	int counts[CellPixels]       = {};
	uint8_t color_of[CellPixels] = {};
	int used                     = 0;

	const auto* cell = source +
	                   static_cast<size_t>(y + ContentYOffset) * source_width + x;
	int position = 0;
	for (int row = 0; row < TextGlyphHeight; ++row) {
		const auto* line = cell + static_cast<size_t>(row) * source_width;
		for (int col = 0; col < TextGlyphWidth; ++col, ++position) {
			const auto color = line[col];
			int i = 0;
			for (; i < used; ++i) {
				if (colors[i] == color) {
					break;
				}
			}
			if (i == used) {
				if (used >= MaxTextCellColors) {
					return false;
				}
				colors[used++] = color;
			}
			++counts[i];
			color_of[position] = static_cast<uint8_t>(i);
		}
	}

	int best            = -1;
	int best_score      = -1;
	for (int c = 0; c < used; ++c) {
		int on_glyph  = 0;
		int off_glyph = 0;
		for (int i = 0; i < CellPixels; ++i) {
			if (color_of[i] != c) {
				continue;
			}
			if (mask[i / 8] & (0x80u >> (i % 8))) {
				++on_glyph;
			} else {
				++off_glyph;
			}
		}
		// The ink has to fill the glyph closely. The pointer paints a small
		// outline over the paper, so spare pixels are tolerated much more
		// loosely than the glyph itself.
		if (on_glyph * 100 < mask_pixels * 55) {
			continue;
		}
		if (off_glyph * 100 > free_pixels * 60) {
			continue;
		}
		const int score = (on_glyph * 100) / mask_pixels -
		                  (off_glyph * 100) / free_pixels;
		if (score > best_score) {
			best_score = score;
			best       = c;
		}
	}
	if (best < 0) {
		return false;
	}
	ink = colors[best];

	int shade = -1;
	for (int i = 0; i < used; ++i) {
		if (i != best && (shade < 0 || counts[i] > counts[shade])) {
			shade = i;
		}
	}
	if (shade < 0) {
		return false;
	}
	background = colors[shade];
	return background != ink;
}

// The game writes its text in whole lines, so one line is painted with one ink
// and one paper. When the pointer hides a cell so much that its own pixels can
// no longer name the character, the glyph the renderer blitted there is still
// correct - the colours of the cell beside it say how to paint it.
//
// Borrowing is only safe when the cell really belongs to that line, so plenty
// of its pixels have to carry the borrowed paper or ink (the pointer adds at
// most a few dozen colours of its own).
//
// Colours alone are not enough: an empty cell of a black menu bar is made of
// exactly the paper and ink a cell beside it names, so a cell that no longer
// holds any text would be painted with the character of the previous screen and
// leave it behind as a ghost. The glyph the renderer blitted there is therefore
// checked as well: some of its strokes have to be visible through the pointer,
// and the paper has to show beside them, exactly as the stored shape expects.
constexpr int BorrowedColourRatio = 70;
// The pointer outline is thin and its inside is transparent, so a glyph it
// covers still shows its own strokes. Empty paper shows none, and the few
// pixels of the outline that happen to fall on the shape are far too few to
// pass this.
constexpr int BorrowedMinInkPixels   = 6;
constexpr int BorrowedMinInkRatio    = 20;
constexpr int BorrowedMinPaperRatio  = 50;

bool text_cell_matches_colors(const uint32_t* source, const int source_width,
                              const uint16_t x, const uint16_t y,
                              const GlyphMask& mask, const uint32_t background,
                              const uint32_t ink)
{
	constexpr int CellPixels = TextGlyphWidth * TextGlyphHeight;

	if (background == ink) {
		return false;
	}
	int mask_pixels = 0;
	for (int i = 0; i < CellPixels; ++i) {
		if (mask[i / 8] & (0x80u >> (i % 8))) {
			++mask_pixels;
		}
	}
	if (mask_pixels < MinGlyphInkPixels || mask_pixels > MaxGlyphInkPixels) {
		return false;
	}
	const int free_pixels = CellPixels - mask_pixels;

	int expected                      = 0;
	int ink_on_glyph                  = 0;
	int paper_off_glyph               = 0;
	uint32_t colors[MaxTextCellColors] = {};
	int used                           = 0;

	const auto* cell = source +
	                   static_cast<size_t>(y + ContentYOffset) * source_width + x;
	for (int row = 0; row < TextGlyphHeight; ++row) {
		const auto* line = cell + static_cast<size_t>(row) * source_width;
		for (int col = 0; col < TextGlyphWidth; ++col) {
			const auto color  = line[col];
			const bool on_glyph = mask[(row * TextGlyphWidth + col) / 8] &
			                      (0x80u >> ((row * TextGlyphWidth + col) % 8));
			if (color == ink && on_glyph) {
				++ink_on_glyph;
			} else if (color == background && !on_glyph) {
				++paper_off_glyph;
			}
			if (color == background || color == ink) {
				++expected;
				continue;
			}
			int i = 0;
			for (; i < used; ++i) {
				if (colors[i] == color) {
					break;
				}
			}
			if (i == used) {
				if (used == MaxTextCellColors) {
					return false;
				}
				colors[used++] = color;
			}
		}
	}
	if (ink_on_glyph < BorrowedMinInkPixels) {
		return false;
	}
	if (ink_on_glyph * 100 < mask_pixels * BorrowedMinInkRatio) {
		return false;
	}
	if (paper_off_glyph * 100 < free_pixels * BorrowedMinPaperRatio) {
		return false;
	}
	return expected * 100 >= CellPixels * BorrowedColourRatio;
}

// The game paints its own mouse pointer into the same framebuffer as the text,
// so a simplified tile would hide it. Every pixel that does not carry the
// expected glyph colour (ink on the glyph, background everywhere else) can only
// come from the pointer, because the cell held nothing but that glyph when it
// was registered. Those pixels are copied back unchanged, so the pointer stays
// on top of the simplified text instead of disappearing underneath it.
void restore_pointer_pixels(const uint32_t* source, const int source_width,
                            uint32_t* destination,
                            const int destination_stride, const uint16_t x,
                            const uint16_t y, const GlyphMask& mask,
                            const uint32_t background, const uint32_t ink)
{
	const auto* cell = source +
	                   static_cast<size_t>(y + ContentYOffset) * source_width + x;
	for (int row = 0; row < TextGlyphHeight; ++row) {
		const auto* line = cell + static_cast<size_t>(row) * source_width;
		auto* dst_line   = destination +
		                 static_cast<size_t>((y + ContentYOffset) * HdScale +
		                                     row * HdScale) *
		                     destination_stride +
		                 static_cast<size_t>(x) * HdScale;
		for (int col = 0; col < TextGlyphWidth; ++col) {
			const int position = row * TextGlyphWidth + col;
			const bool on_glyph =
			        (mask[position / 8] & (0x80u >> (position % 8))) != 0;
			const auto color = line[col];
			if (color == (on_glyph ? ink : background)) {
				continue;
			}
			for (int dy = 0; dy < HdScale; ++dy) {
				auto* dst = dst_line +
				            static_cast<size_t>(dy) * destination_stride +
				            col * HdScale;
				for (int dx = 0; dx < HdScale; ++dx) {
					dst[dx] = color;
				}
			}
		}
	}
}

const sea2_glyph_map::Entry* glyph_map_entry(const uint16_t record)
{
	const auto index = record - sea2_glyph_map::FirstRecord;
	if (index >= sea2_glyph_map::RecordCount) {
		return nullptr;
	}
	return &sea2_glyph_map::kEntries[index];
}

uint16_t glyph_record_for(const uint32_t simplified)
{
	for (uint16_t i = 0; i < sea2_glyph_map::RecordCount; ++i) {
		if (sea2_glyph_map::kEntries[i].simplified == simplified) {
			return static_cast<uint16_t>(sea2_glyph_map::FirstRecord + i);
		}
	}
	return 0;
}

// A word of the original translation can stay the wrong word after every
// character of it was converted to its simplified form, because the conversion
// works one character at a time: the Taiwanese name "義大利" becomes "义大利",
// while the mainland name of the country is "意大利". Only the character that
// differs is replaced, and only when the rest of the word is on the very same
// line, so that no other occurrence of 义 can be changed by accident.
struct TextWordPatch {
	uint32_t first;       // simplified character that is replaced
	uint32_t second;      // the character that follows it in the word
	uint32_t third;       // and the one after that
	uint32_t replacement; // what the first character has to become
};

constexpr std::array<TextWordPatch, 1> TextWordPatches = {{
	{0x4E49, 0x5927, 0x5229, 0x610F}, // 义大利 -> 意大利
}};

// The character that follows the given one on the same line, as far as the
// records of this screen are concerned. The word is patched from the records
// pass one filled in, so the replacement stays in place even while the pointer
// covers the neighbouring characters.
const TextCell* next_text_cell(const TextCell& cell, const uint16_t after_x,
                              const uint32_t simplified)
{
	const TextCell* found = nullptr;
	for (const auto& other : text_cells) {
		if (&other == &cell || !other.known || !other.verified) {
			continue;
		}
		if (std::abs(static_cast<int>(other.y) - static_cast<int>(cell.y)) > 1) {
			continue;
		}
		const int distance = static_cast<int>(other.x) - static_cast<int>(after_x);
		if (distance <= 0 || distance > TextGlyphWidth * 2) {
			continue;
		}
		const auto* entry = glyph_map_entry(other.record);
		if (!entry || entry->simplified != simplified) {
			continue;
		}
		if (!found || other.x < found->x) {
			found = &other;
		}
	}
	return found;
}

uint16_t patched_text_record(const TextCell& cell, const uint16_t record)
{
	const auto* entry = glyph_map_entry(record);
	if (!entry) {
		return record;
	}
	for (const auto& patch : TextWordPatches) {
		if (entry->simplified != patch.first) {
			continue;
		}
		const auto* second = next_text_cell(cell, cell.x, patch.second);
		if (!second) {
			continue;
		}
		const auto* third = next_text_cell(*second, second->x, patch.third);
		if (!third) {
			continue;
		}
		const auto replacement = glyph_record_for(patch.replacement);
		if (replacement != 0) {
			return replacement;
		}
	}
	return record;
}

bool draw_text_cell(const uint16_t record, const uint16_t x, const uint16_t y,
                    const uint32_t background, const uint32_t ink,
                    uint32_t* destination, const int destination_stride)
{
	if (background == ink) {
		return true;
	}
	const auto* atlas = get_glyph_atlas();
	if (!atlas) {
		return false;
	}
	const auto index = record - sea2_glyph_map::FirstRecord;
	if (index >= atlas->available.size() || !atlas->available[index]) {
		return false;
	}

	const auto tile_x = static_cast<int>(index % GlyphAtlasColumns) * GlyphTileWidth;
	const auto tile_y = static_cast<int>(index / GlyphAtlasColumns) * GlyphTileHeight;

	const auto br = static_cast<uint8_t>((background >> 16) & 0xff);
	const auto bg = static_cast<uint8_t>((background >> 8) & 0xff);
	const auto bb = static_cast<uint8_t>(background & 0xff);
	const auto ir = static_cast<uint8_t>((ink >> 16) & 0xff);
	const auto ig = static_cast<uint8_t>((ink >> 8) & 0xff);
	const auto ib = static_cast<uint8_t>(ink & 0xff);

	for (int row = 0; row < GlyphTileHeight; ++row) {
		const auto* alpha_row = atlas->alpha.data() +
		                        static_cast<size_t>(tile_y + row) * atlas->width +
		                        tile_x;
		auto* dst = destination +
		            (static_cast<size_t>(y + ContentYOffset) * HdScale + row) *
		                    destination_stride +
		            static_cast<size_t>(x) * HdScale;
		for (int col = 0; col < GlyphTileWidth; ++col) {
			const auto alpha = alpha_row[col];
			dst[col]        = Bgrx8888(blend_channel(ir, br, alpha),
			                           blend_channel(ig, bg, alpha),
			                           blend_channel(ib, bb, alpha));
		}
	}
	return true;
}

// Temporary: whether the atlas carries a simplified glyph for this record at
// all, so a hovered cell that keeps the game's own pixels can be told apart
// from one that is drawn and simply looks the same.
bool has_simplified_glyph(const uint16_t record)
{
	const auto* atlas = get_glyph_atlas();
	if (!atlas) {
		return false;
	}
	const auto index = record - sea2_glyph_map::FirstRecord;
	return index < atlas->available.size() && atlas->available[index];
}

} // namespace

uint16_t sea2_hd_trace_loadseg = 0;
bool sea2_hd_string_trace = false;
uint32_t sea2_hd_glyph_watch = 0;

// One-off survey of how a screen is painted: every generic blit is tallied per
// host frame and written out as one line when the frame ends. A screen painted
// as one big rectangle and a screen painted tile by tile therefore look
// completely different, which is the only thing this survey has to tell.
// Only used with `sea2_hd = trace`.
std::FILE* blit_seq_file          = nullptr;
uint32_t blit_seq_lines           = 0;
uint32_t blit_seq_frames          = 0;
uint32_t blit_seq_total           = 0;
std::unordered_map<uint32_t, uint32_t> blit_seq_shapes = {};
// String instructions are the other way a screen gets built: a tiled background
// is copied plane by plane with `rep movs`, and a whole picture with a single
// long one. Both cases are counted per frame so the two can be told apart even
// when neither blitter entry is involved.
uint32_t blit_seq_string_ops      = 0;
uint32_t blit_seq_string_bytes    = 0;
uint32_t blit_seq_string_es_min   = 0xffffffffu;
uint32_t blit_seq_string_es_max   = 0;
// A `rep movs` that ran out of cycles is entered again with a smaller CX, so
// only the largest count seen at one site during a frame is added up.
std::unordered_map<uint32_t, uint32_t> blit_seq_string_max = {};
std::chrono::steady_clock::time_point blit_seq_clock_start = {};
constexpr uint32_t BlitSeqMaxLines = 200000;
constexpr uint32_t BlitSeqFullScreenArea = 320u * 200u;

// Frame dumping: `sea2_hd_frame_dump = N` writes every Nth frame to
// `sea2_frames/f_NNNNN.png`, which is the only way to tell which moment of a
// trace shows the screen that is being investigated.
int sea2_hd_frame_dump_interval = 0;
uint32_t sea2_hd_frame_dumps    = 0;
constexpr uint32_t FrameDumpMax = 240;

void blit_seq(const char* format, ...)
{
	if (!sea2_hd_string_trace || blit_seq_lines >= BlitSeqMaxLines) {
		return;
	}
	if (!blit_seq_file) {
		blit_seq_file = std::fopen("sea2_blit_seq.txt", "w");
		if (!blit_seq_file) {
			return;
		}
	}
	va_list args;
	va_start(args, format);
	std::vfprintf(blit_seq_file, format, args);
	va_end(args);
	std::fputc('\n', blit_seq_file);
	++blit_seq_lines;
	if ((blit_seq_lines & 0x3ff) == 0) {
		std::fflush(blit_seq_file);
	}
}

void blit_seq_add(uint16_t width, uint16_t height)
{
	if (!sea2_hd_string_trace) {
		return;
	}
	++blit_seq_total;
	++blit_seq_shapes[(static_cast<uint32_t>(width) << 16) | height];
}

// Runs once per host frame, before the next one is painted. Only the shapes
// that make up the frame are listed, most frequent first, so a 16x16 tile grid
// is immediately recognisable and a single 640x480 rectangle even more so.
// Frames that painted nothing are skipped, which keeps the log small; a screen
// that is only painted once still shows up in exactly the frame it appears.
void blit_seq_flush_frame()
{
	if (!sea2_hd_string_trace) {
		return;
	}
	if (blit_seq_total == 0 && blit_seq_string_ops == 0) {
		return;
	}
	const uint32_t total = blit_seq_total;
	blit_seq_total       = 0;

	std::vector<std::pair<uint32_t, uint32_t>> shapes;
	shapes.reserve(blit_seq_shapes.size());
	for (const auto& entry : blit_seq_shapes) {
		shapes.emplace_back(entry.second, entry.first);
	}
	blit_seq_shapes.clear();
	std::sort(shapes.begin(), shapes.end(),
	          [](const auto& a, const auto& b) { return a.first > b.first; });

	bool full_screen = false;
	std::string text;
	char item[64] = {};
	size_t listed = 0;
	for (const auto& entry : shapes) {
		const uint16_t w = static_cast<uint16_t>(entry.second >> 16);
		const uint16_t h = static_cast<uint16_t>(entry.second & 0xffff);
		if (static_cast<uint32_t>(w) * h >= BlitSeqFullScreenArea) {
			full_screen = true;
		}
		if (listed >= 14) {
			continue;
		}
		++listed;
		snprintf(item, sizeof(item), "%s%ux%u:%u",
		         text.empty() ? "" : ",", w, h, entry.first);
		text += item;
	}

	std::string es_text = "-";
	if (blit_seq_string_ops != 0) {
		snprintf(item, sizeof(item), "%05x-%05x",
		         static_cast<unsigned>(blit_seq_string_es_min),
		         static_cast<unsigned>(blit_seq_string_es_max));
		es_text = item;
	}

	const auto now = std::chrono::steady_clock::now();
	if (blit_seq_clock_start == std::chrono::steady_clock::time_point{}) {
		blit_seq_clock_start = now;
	}
	const double elapsed =
	        std::chrono::duration<double>(now - blit_seq_clock_start).count();

	blit_seq("FH f=%u t=%.1f n=%u fs=%u so=%u sb=%u es=%s s=%s",
	         blit_seq_frames, elapsed, total, full_screen ? 1 : 0,
	         blit_seq_string_ops, blit_seq_string_bytes, es_text.c_str(),
	         text.c_str());

	blit_seq_string_ops    = 0;
	blit_seq_string_bytes  = 0;
	blit_seq_string_es_min = 0xffffffffu;
	blit_seq_string_es_max = 0;
	blit_seq_string_max.clear();
}

void SEA2_HD_TraceGlyphSite(const uint16_t cs, const uint16_t ip)
{
	size_t slot = (static_cast<size_t>(cs) * 13u + static_cast<size_t>(ip) * 149u) %
	              GlyphSiteTableSize;
	for (size_t probe = 0; probe < GlyphSiteTableSize; ++probe) {
		auto& entry = glyph_site_table[slot];
		if (!entry.used) {
			entry.used = true;
			entry.cs = cs;
			entry.ip = ip;
			entry.count = 0;
		}
		if (entry.cs == cs && entry.ip == ip) {
			++entry.count;
			return;
		}
		slot = (slot + 1) % GlyphSiteTableSize;
	}
}

void SEA2_HD_TraceStringOp(const uint16_t cs, const uint16_t ip, const uint16_t ds,
                           const uint16_t si, const uint16_t es, const uint16_t di,
                           const uint16_t cx, const uint8_t opcode)
{
	// Only the instructions that write to ES:DI can build a screen, so just
	// those are tallied per frame, together with where they write.
	if (opcode == 0xa4 || opcode == 0xa5 || opcode == 0xaa || opcode == 0xab) {
		const uint32_t bytes = static_cast<uint32_t>(cx) *
		                       ((opcode == 0xa5 || opcode == 0xab) ? 2u : 1u);
		const uint32_t phys = (static_cast<uint32_t>(es) << 4) + di;
		++blit_seq_string_ops;
		blit_seq_string_es_min = std::min(blit_seq_string_es_min, phys);
		blit_seq_string_es_max = std::max(blit_seq_string_es_max, phys);
		const uint32_t key = (static_cast<uint32_t>(cs) << 16) | ip;
		uint32_t& seen    = blit_seq_string_max[key];
		if (bytes > seen) {
			blit_seq_string_bytes += bytes - seen;
			seen = bytes;
		}
	}

	size_t slot = (static_cast<size_t>(cs) * 31u + static_cast<size_t>(ip) * 7u +
	               static_cast<size_t>(ds)) %
	              StringOpTableSize;
	for (size_t probe = 0; probe < StringOpTableSize; ++probe) {
		auto& entry = string_op_table[slot];
		if (!entry.used) {
			entry.used = true;
			entry.cs = cs;
			entry.ip = ip;
			entry.ds = ds;
			entry.opcode = opcode;
			entry.count = 0;
			entry.min_si = 0xffffffff;
			entry.max_si = 0;
			entry.min_di = 0xffffffff;
			entry.max_di = 0;
		}
		if (entry.cs == cs && entry.ip == ip && entry.ds == ds) {
			++entry.count;
			entry.min_si = std::min(entry.min_si, static_cast<uint32_t>(si));
			entry.max_si = std::max(entry.max_si, static_cast<uint32_t>(si));
			entry.min_di = std::min(entry.min_di, static_cast<uint32_t>(di));
			entry.max_di = std::max(entry.max_di, static_cast<uint32_t>(di));
			if (++string_op_total >= StringOpDumpEveryOps) {
				string_op_total = 0;
				SEA2_HD_DumpStringTrace();
			}
			return;
		}
		slot = (slot + 1) % StringOpTableSize;
	}
}

void SEA2_HD_TraceBlit(const uint16_t ds, const uint16_t src, const uint16_t x,
                       const uint16_t y, const uint16_t width,
                       const uint16_t height, const uint16_t planes)
{
	blit_seq_add(width, height);

	size_t slot = (static_cast<size_t>(width) * 37u +
	               static_cast<size_t>(height) * 101u) %
	              BlitShapeTableSize;
	for (size_t probe = 0; probe < BlitShapeTableSize; ++probe) {
		auto& entry = blit_shape_table[slot];
		if (!entry.used) {
			entry = {};
			entry.used = true;
			entry.width = width;
			entry.height = height;
			entry.ds = ds;
		}
		if (entry.width == width && entry.height == height) {
			++entry.count;
			entry.min_src = std::min(entry.min_src, src);
			entry.max_src = std::max(entry.max_src, src);
			entry.min_x = std::min(entry.min_x, x);
			entry.max_x = std::max(entry.max_x, x);
			entry.min_y = std::min(entry.min_y, y);
			entry.max_y = std::max(entry.max_y, y);
			break;
		}
		slot = (slot + 1) % BlitShapeTableSize;
	}

	if (width != 2 || height != 16) {
		return;
	}
	slot = (static_cast<size_t>(ds) * 131u + static_cast<size_t>(src) * 3u) %
	       GlyphBlitTableSize;
	for (size_t probe = 0; probe < GlyphBlitTableSize; ++probe) {
		auto& entry = glyph_blit_table[slot];
		if (!entry.used) {
			entry.used = true;
			entry.ds = ds;
			entry.src = src;
			entry.count = 0;
			entry.min_x = 0xffff;
			entry.max_x = 0;
			entry.min_y = 0xffff;
			entry.max_y = 0;
		}
		if (entry.ds == ds && entry.src == src) {
			++entry.count;
			entry.min_x = std::min(entry.min_x, x);
			entry.max_x = std::max(entry.max_x, x);
			entry.min_y = std::min(entry.min_y, y);
			entry.max_y = std::max(entry.max_y, y);
			if (++glyph_blit_total >= StringOpDumpEveryOps) {
				glyph_blit_total = 0;
				SEA2_HD_DumpStringTrace();
			}
			return;
		}
		slot = (slot + 1) % GlyphBlitTableSize;
	}
}

void SEA2_HD_ScanFont()
{
	static std::array<bool, FontSignatureCount> reported = {};
	static uint32_t scan_count = 0;

	bool all_found = true;
	for (const auto found : reported) {
		if (!found) {
			all_found = false;
			break;
		}
	}
	if (all_found || scan_count >= 60) {
		return;
	}
	++scan_count;

	constexpr uint32_t Limit = 0x100000;
	for (uint32_t phys = 0; phys + 32 < Limit; phys += 2) {
		const uint8_t first = mem_readb(phys);
		for (size_t i = 0; i < FontSignatureCount; ++i) {
			if (reported[i] || first != font_signatures[i].bytes[0]) {
				continue;
			}
			size_t k = 1;
			for (; k < 32; ++k) {
				if (mem_readb(phys + k) != font_signatures[i].bytes[k]) {
					break;
				}
			}
			if (k != 32) {
				continue;
			}
			reported[i] = true;
			LOG_MSG("SEA2HD: font glyph %u at phys=%05X (seg=%04X off=%04X)",
			        font_signatures[i].glyph, phys, phys >> 4, phys & 0xf);
			if (std::FILE* file = std::fopen("sea2_font_scan.txt", "at")) {
				std::fprintf(file, "glyph=%u phys=%05X seg=%04X off=%04X\n",
				             font_signatures[i].glyph, phys, phys >> 4,
				             phys & 0xf);
				std::fclose(file);
			}
		}
	}
}

void SEA2_HD_TraceDosOpen(const char* name, const uint16_t handle)
{
	if ((!sea2_hd_string_trace && !hd_enabled) || !name) {
		return;
	}
	// Track the font file handle so every one of its reads gets logged,
	// including 32-byte on-demand glyph reads.
	std::string lower{name};
	std::transform(lower.begin(), lower.end(), lower.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (lower.find("1.pat") != std::string::npos) {
		sea2_font_handle = handle;
	}
	if (!sea2_hd_string_trace) {
		return;
	}
	if (std::FILE* file = std::fopen("sea2_dos_trace.txt", "at")) {
		std::fprintf(file, "open handle=%04X name=%s\n", handle, name);
		std::fclose(file);
	}
}

void SEA2_HD_TraceDosRead(const uint16_t ds, const uint16_t dx,
                          const uint16_t len, const uint16_t handle,
                          const uint32_t file_offset)
{
	if (!sea2_hd_string_trace && !hd_enabled) {
		return;
	}
	if (len < 16) {
		return;
	}
	if (handle == sea2_font_handle && len == 28) {
		// 1.PAT stores glyphs in 512-byte sectors: 18 glyphs of 28 bytes
		// followed by 8 bytes of zero padding. There is no file header, so
		// the glyph index is (sector * 18) + (offset % 512) / 28.
		constexpr uint32_t SectorSize      = 512;
		constexpr uint32_t GlyphsPerSector = 18;
		constexpr uint32_t GlyphRecordSize = 28;
		const uint32_t sector = file_offset / SectorSize;
		const uint32_t within = file_offset % SectorSize;
		if (within < GlyphsPerSector * GlyphRecordSize &&
		    within % GlyphRecordSize == 0) {
			const uint32_t record = sector * GlyphsPerSector +
			                        within / GlyphRecordSize;
			if (record >= sea2_glyph_map::FirstRecord &&
			    record < sea2_glyph_map::FirstRecord +
			                     sea2_glyph_map::RecordCount) {
				// The next text blit consumes this record.
				pending_glyph_record = static_cast<uint16_t>(record);
				// The transfer already landed in DOS memory, so the
				// bitmap can be indexed by content.
				if (hd_enabled) {
					std::array<uint8_t, TextGlyphBytes> bitmap = {};
					for (int i = 0; i < TextGlyphBytes; ++i) {
						bitmap[i] = mem_readb(PhysicalMake(
						        ds, static_cast<uint16_t>(dx + i)));
					}
					index_glyph_bitmap(static_cast<uint16_t>(record),
					                   bitmap.data());
				}
				if (sea2_hd_string_trace) {
					// Log each distinct record once, with its mapped text.
					static std::array<bool, sea2_glyph_map::RecordCount> seen = {};
					const size_t slot = record - sea2_glyph_map::FirstRecord;
					if (!seen[slot]) {
						seen[slot] = true;
						if (std::FILE* file = std::fopen("sea2_glyph_reads.txt", "at")) {
							std::fprintf(file,
							             "record=%u trad=U+%04X simp=U+%04X\n",
							             static_cast<unsigned>(record),
							             static_cast<unsigned>(SEA2_HD_GlyphTraditional(
							                     static_cast<uint16_t>(record))),
							             static_cast<unsigned>(SEA2_HD_GlyphSimplified(
							                     static_cast<uint16_t>(record))));
							std::fclose(file);
						}
					}
				}
			}
		}
		if (!sea2_hd_string_trace) {
			return;
		}
		if (sea2_hd_trace_loadseg != 0) {
			sea2_hd_glyph_watch = 600;
		}
		const uint16_t ss_seg = SegValue(ss);
		const uint16_t sp = static_cast<uint16_t>(reg_esp);
		if (sea2_hd_trace_loadseg != 0 &&
		    stack_sample_total < StackSampleCount) {
			auto& sample = stack_samples[stack_sample_total++];
			sample.ss = ss_seg;
			sample.sp = sp;
			sample.ax = reg_ax;
			sample.bx = reg_bx;
			sample.cx = reg_cx;
			sample.dx = reg_dx;
			sample.ds = ds;
			for (size_t i = 0; i < sample.words.size(); ++i) {
				sample.words[i] = mem_readw(PhysicalMake(
				        ss_seg, static_cast<uint16_t>(sp + 2 * i)));
			}
			// The return address sits at sp+8; record the surrounding bytes
			// now, while they still hold the caller's code.
			const uint16_t ret_ip_now = sample.words[4];
			const uint16_t ret_cs_now = sample.words[5];
			for (size_t i = 0; i < 32; ++i) {
				const auto address = PhysicalMake(
				        ret_cs_now,
				        static_cast<uint16_t>(ret_ip_now - 8 + i));
				caller_bytes[stack_sample_total - 1][i] =
				        mem_readb(address);
			}
		}
		// The INT 21h frame holds the caller's IP/CS at the top of the stack.
		const uint16_t ret_ip =
		        mem_readw(PhysicalMake(ss_seg, static_cast<uint16_t>(sp + 8)));
		const uint16_t ret_cs =
		        mem_readw(PhysicalMake(ss_seg, static_cast<uint16_t>(sp + 10)));
		size_t slot = (static_cast<size_t>(ret_cs) * 7u +
		               static_cast<size_t>(ret_ip) * 173u) %
		              GlyphCallerTableSize;
		for (size_t probe = 0; probe < GlyphCallerTableSize; ++probe) {
			auto& entry = glyph_caller_table[slot];
			if (!entry.used) {
				entry.used = true;
				entry.cs = ret_cs;
				entry.ip = ret_ip;
				entry.count = 0;
			}
			if (entry.cs == ret_cs && entry.ip == ret_ip) {
				++entry.count;
				break;
			}
			slot = (slot + 1) % GlyphCallerTableSize;
		}
	}
	if (sea2_hd_string_trace) {
		if (std::FILE* file = std::fopen("sea2_dos_trace.txt", "at")) {
			std::fprintf(file, "read handle=%04X ds=%04X dx=%04X len=%u\n",
			             handle, ds, dx, len);
			std::fclose(file);
		}
	}
}

void SEA2_HD_TraceInt21Ring(const uint16_t ss_seg, const uint16_t sp,
                            const uint32_t* history, const unsigned count)
{
	if (!sea2_hd_string_trace) {
		return;
	}
	if (reg_cx != 28 || reg_bx != sea2_font_handle) {
		return;
	}
	// OPEN.EXE draws text with its own copy of the engine; only MAIN.EXE is
	// in scope here, so ignore samples taken while the intro program runs.
	if (sea2_hd_trace_loadseg == 0) {
		return;
	}
	if (int21_sample_total >= Int21SampleCount) {
		return;
	}
	auto& sample = int21_samples[int21_sample_total++];
	sample.ss = ss_seg;
	sample.sp = sp;
	sample.bx = reg_bx;
	sample.cx = reg_cx;
	sample.dx = reg_dx;
	const auto limit = std::min<unsigned>(count, sample.history.size());
	for (unsigned i = 0; i < limit; ++i) {
		sample.history[i] = history[i];
	}
}

// The text renderer converts each Big5 code into a 1-based 1.PAT record
// before it loads the glyph, so the record is known before the blitter runs.
// Capturing it there removes any need to recognise the bitmap later.
void SEA2_HD_NotifyGlyphRecord(const uint16_t record1)
{
	if (!hd_enabled || record1 <= sea2_glyph_map::FirstRecord) {
		return;
	}
	const uint16_t record = static_cast<uint16_t>(record1 - 1);
	if (record >= sea2_glyph_map::FirstRecord +
	                      sea2_glyph_map::RecordCount) {
		return;
	}
	pending_glyph_record = record;
}

void SEA2_HD_TraceBlitterEntry(const uint16_t ip)
{
	if ((!sea2_hd_string_trace && !hd_enabled) || sea2_hd_trace_loadseg == 0) {
		return;
	}
	if (ip != BlitterEntryA && ip != BlitterEntryB) {
		return;
	}
	// Only the text renderer calls through DS:0x8A78; its return address is
	// 0x4842, right after the indirect far call.
	const uint16_t sp = static_cast<uint16_t>(reg_esp);
	const uint16_t ss_seg = SegValue(ss);
	const uint16_t ret_ip = mem_readw(PhysicalMake(ss_seg, sp));
	const uint16_t ret_cs = mem_readw(
	        PhysicalMake(ss_seg, static_cast<uint16_t>(sp + 2)));
	const bool text_caller =
	        ret_ip == 0x4842 && ret_cs == sea2_hd_trace_loadseg;

	if (sea2_hd_string_trace) {
		// Arguments sit above the return address because the hook runs before
		// the prologue's `push bp`. The 2x14 glyph blits are already described
		// by the text pipeline, so only the others are worth a line here.
		const uint16_t sx = mem_readw(PhysicalMake(ss_seg, sp + 4));
		const uint16_t sy = mem_readw(PhysicalMake(ss_seg, sp + 6));
		const uint16_t sw = mem_readw(PhysicalMake(ss_seg, sp + 8));
		const uint16_t sh = mem_readw(PhysicalMake(ss_seg, sp + 10));
		if (sw != 2 || sh != 14) {
			blit_seq_add(sw, sh);
		}
	}

	if (hd_enabled) {
		// Arguments sit above the return address because the hook runs
		// before the prologue's `push bp`.
		const uint16_t x = mem_readw(PhysicalMake(ss_seg, sp + 4));
		const uint16_t y = mem_readw(PhysicalMake(ss_seg, sp + 6));
		const uint16_t width = mem_readw(PhysicalMake(ss_seg, sp + 8));
		const uint16_t height = mem_readw(PhysicalMake(ss_seg, sp + 10));
		const uint16_t source = mem_readw(PhysicalMake(ss_seg, sp + 14));
		if (width == 2 && height == 14) {
			const uint16_t ds_seg = SegValue(ds);
			std::array<uint8_t, TextGlyphBytes> bitmap = {};
			for (int i = 0; i < TextGlyphBytes; ++i) {
				bitmap[i] = mem_readb(PhysicalMake(
				        ds_seg, static_cast<uint16_t>(source + i)));
			}
			// The renderer announces the record before every wide glyph, so
			// the blit only has to consume it. The bitmap lookup stays as a
			// fallback for any path that does not go through the mapper.
			const uint16_t announced_record = pending_glyph_record;
			const bool announced = announced_record != 0;
			pending_glyph_record = 0;
			const auto record = lookup_glyph_bitmap(bitmap.data());
			const auto cell_record = record != 0 ? record : announced_record;
			// The shape the renderer used is taken from the font itself when
			// the record is known, so a blit whose source pointer could not be
			// read still leaves a usable reference behind.
			GlyphMask cell_mask = {};
			std::memcpy(cell_mask.data(), bitmap.data(), TextGlyphBytes);
			const auto mask_it = record_masks.find(cell_record);
			if (cell_record != 0 && mask_it != record_masks.end()) {
				cell_mask = mask_it->second;
			}
			// Registering the cell is the only thing this hook does for HD:
			// the glyph itself is identified again from the framebuffer on
			// every composited frame.
			if (!is_blank_glyph_bitmap(bitmap.data()) &&
			    (announced || record != 0 || text_caller)) {
				register_text_cell(static_cast<uint16_t>(x * 8), y,
				                   cell_record, cell_mask);
			}
			if (record == 0 && text_caller) {
				++glyph_miss_total;
				if (glyph_miss_total <= 8) {
					LOG_MSG("SEA2HD: text miss x=%u y=%u src=%u ds=%u",
					        static_cast<unsigned>(x * 8),
					        static_cast<unsigned>(y),
					        static_cast<unsigned>(source),
					        static_cast<unsigned>(SegValue(ds)));
				}
			}
		}
	}
	if (!sea2_hd_string_trace || !text_caller) {
		return;
	}
	++blitter_entry_total;
	if (blitter_sample_total >= BlitterSampleCount) {
		return;
	}
	auto& s = blitter_samples[blitter_sample_total++];
	s.ip = ip;
	s.bp = static_cast<uint16_t>(reg_ebp);
	s.ax = reg_ax;
	s.bx = reg_bx;
	s.cx = reg_cx;
	s.dx = reg_dx;
	s.si = reg_si;
	s.di = reg_di;
	s.es = SegValue(es);
	s.ds = SegValue(ds);
	// The hook runs before the prologue's `push bp`, so the arguments still
	// sit above the return address: [sp+4] is the first parameter.
	for (size_t i = 0; i < s.args.size(); ++i) {
		s.args[i] = mem_readw(PhysicalMake(
		        ss_seg, static_cast<uint16_t>(sp + 4 + 2 * i)));
	}
	const uint16_t src = s.args[5];
	for (size_t i = 0; i < s.glyph.size(); ++i) {
		s.glyph[i] = mem_readb(PhysicalMake(
		        s.ds, static_cast<uint16_t>(src + i)));
	}
}

void SEA2_HD_TraceInt16(const uint16_t cs, const uint16_t ip, const uint8_t ah,
                        const uint16_t ax)
{
	if (!sea2_hd_string_trace || sea2_hd_trace_loadseg == 0) {
		return;
	}
	++key_site_total;
	size_t slot = (static_cast<size_t>(cs) * 7u +
	               static_cast<size_t>(ip) * 173u) %
	              KeySiteTableSize;
	for (size_t probe = 0; probe < KeySiteTableSize; ++probe) {
		auto& entry = key_site_table[slot];
		if (!entry.used) {
			entry.used = true;
			entry.cs = cs;
			entry.ip = ip;
			entry.count = 0;
		}
		if (entry.cs == cs && entry.ip == ip) {
			++entry.count;
			entry.last_ah = ah;
			entry.last_ax = ax;
			break;
		}
		slot = (slot + 1) % KeySiteTableSize;
	}
}

void SEA2_HD_TraceDosCall(const uint16_t cs, const uint16_t ip, const uint8_t ah)
{
	if (!sea2_hd_string_trace || sea2_hd_trace_loadseg == 0) {
		return;
	}
	++dos_call_total;
	size_t slot = (static_cast<size_t>(cs) * 7u +
	               static_cast<size_t>(ip) * 173u +
	               static_cast<size_t>(ah) * 31u) %
	              DosCallTableSize;
	for (size_t probe = 0; probe < DosCallTableSize; ++probe) {
		auto& entry = dos_call_table[slot];
		if (!entry.used) {
			entry.used = true;
			entry.cs = cs;
			entry.ip = ip;
			entry.ah = ah;
			entry.count = 0;
		}
		if (entry.cs == cs && entry.ip == ip && entry.ah == ah) {
			++entry.count;
			break;
		}
		slot = (slot + 1) % DosCallTableSize;
	}
}

void SEA2_HD_ProfileIp(const uint16_t cs, const uint16_t ip)
{
	if (!sea2_hd_string_trace || sea2_hd_trace_loadseg == 0) {
		return;
	}
	++hot_site_total;
	size_t slot = (static_cast<size_t>(cs) * 7u +
	               static_cast<size_t>(ip) * 173u) %
	              HotSiteTableSize;
	for (size_t probe = 0; probe < HotSiteTableSize; ++probe) {
		auto& entry = hot_site_table[slot];
		if (!entry.used) {
			entry.used = true;
			entry.cs = cs;
			entry.ip = ip;
			entry.count = 0;
		}
		if (entry.cs == cs && entry.ip == ip) {
			++entry.count;
			break;
		}
		slot = (slot + 1) % HotSiteTableSize;
	}
}

void SEA2_HD_TraceEntry(const uint16_t cs, const uint16_t ip, const uint16_t ss,
                        const uint16_t sp, const uint16_t* words,
                        const unsigned count)
{
	if (!sea2_hd_string_trace || sea2_hd_trace_loadseg == 0) {
		return;
	}
	if (entry_sample_total >= EntrySampleCount) {
		return;
	}
	auto& sample = entry_samples[entry_sample_total++];
	sample.cs = cs;
	sample.ip = ip;
	sample.ss = ss;
	sample.sp = sp;
	const auto limit = std::min<unsigned>(count, sample.words.size());
	for (unsigned i = 0; i < limit; ++i) {
		sample.words[i] = words[i];
	}
}

void SEA2_HD_DumpStringTrace()
{
	std::FILE* file = std::fopen("sea2_string_trace.txt", "wt");
	if (!file) {
		return;
	}
	std::fprintf(file, "cs:ip ds op count min_si max_si span min_di max_di\n");
	for (const auto& entry : string_op_table) {
		if (!entry.used || entry.count == 0) {
			continue;
		}
		const uint32_t span = entry.max_si - entry.min_si;
		std::fprintf(file,
		             "%04X:%04X %04X %02X %u %04X %04X %u %04X %04X\n",
		             entry.cs, entry.ip, entry.ds, entry.opcode, entry.count,
		             entry.min_si, entry.max_si, span, entry.min_di,
		             entry.max_di);
	}
	std::fclose(file);

	file = std::fopen("sea2_glyph_trace.txt", "wt");
	if (!file) {
		return;
	}
	std::fprintf(file, "ds src count min_x max_x min_y max_y\n");
	for (const auto& entry : glyph_blit_table) {
		if (!entry.used || entry.count == 0) {
			continue;
		}
		std::fprintf(file, "%04X %04X %u %u %u %u %u\n", entry.ds, entry.src,
		             entry.count, entry.min_x, entry.max_x, entry.min_y,
		             entry.max_y);
	}
	std::fclose(file);

	file = std::fopen("sea2_blit_trace.txt", "wt");
	if (!file) {
		return;
	}
	std::fprintf(file,
	             "w h count ds min_src max_src min_x max_x min_y max_y\n");
	for (const auto& entry : blit_shape_table) {
		if (!entry.used || entry.count == 0) {
			continue;
		}
		std::fprintf(file, "%u %u %u %04X %04X %04X %u %u %u %u\n",
		             entry.width, entry.height, entry.count, entry.ds,
		             entry.min_src, entry.max_src, entry.min_x, entry.max_x,
		             entry.min_y, entry.max_y);
	}
	std::fclose(file);

	file = std::fopen("sea2_glyph_sites.txt", "wt");
	if (!file) {
		return;
	}
	std::fprintf(file, "cs:ip count\n");
	std::vector<std::pair<uint32_t, std::pair<uint16_t, uint16_t>>> sites;
	for (const auto& entry : glyph_site_table) {
		if (entry.used && entry.count > 0) {
			sites.emplace_back(entry.count, std::make_pair(entry.cs, entry.ip));
		}
	}
	std::sort(sites.begin(), sites.end(),
	          [](const auto& a, const auto& b) { return a.first > b.first; });
	for (const auto& site : sites) {
		std::fprintf(file, "%04X:%04X %u\n", site.second.first,
		             site.second.second, site.first);
	}
	std::fclose(file);

	file = std::fopen("sea2_glyph_callers.txt", "wt");
	if (!file) {
		return;
	}
	std::fprintf(file, "ret_cs:ret_ip count\n");
	std::vector<std::pair<uint32_t, std::pair<uint16_t, uint16_t>>> callers;
	for (const auto& entry : glyph_caller_table) {
		if (entry.used && entry.count > 0) {
			callers.emplace_back(entry.count,
			                     std::make_pair(entry.cs, entry.ip));
		}
	}
	std::sort(callers.begin(), callers.end(),
	          [](const auto& a, const auto& b) { return a.first > b.first; });
	for (const auto& c : callers) {
		std::fprintf(file, "%04X:%04X %u\n", c.second.first, c.second.second,
		             c.first);
	}
	std::fclose(file);

	file = std::fopen("sea2_glyph_stack.txt", "wt");
	if (!file) {
		return;
	}
	for (size_t i = 0; i < stack_sample_total; ++i) {
		const auto& s = stack_samples[i];
		std::fprintf(file,
		             "ss=%04X sp=%04X ax=%04X bx=%04X cx=%04X dx=%04X ds=%04X "
		             "stack:",
		             s.ss, s.sp, s.ax, s.bx, s.cx, s.dx, s.ds);
		for (const auto w : s.words) {
			std::fprintf(file, " %04X", w);
		}
		std::fprintf(file, "\n");
	}
	std::fclose(file);

	// Dump the bytes that live at the reported caller address, so the mapping
	// between the on-disk image and the runtime code can be checked directly.
	file = std::fopen("sea2_glyph_caller_code.txt", "wt");
	if (!file) {
		return;
	}
	{
		std::vector<std::pair<uint16_t, uint16_t>> seen;
		for (size_t i = 0; i < stack_sample_total; ++i) {
			const auto cs = stack_samples[i].words[5];
			const auto ip = stack_samples[i].words[4];
			const auto key = std::make_pair(cs, ip);
			if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
				continue;
			}
			seen.push_back(key);
			std::fprintf(file, "caller %04X:%04X\n", cs, ip);
			for (int row = -0x80; row < 0x180; row += 16) {
				const auto off = static_cast<uint16_t>(ip + row);
				std::fprintf(file, "%04X:", off);
				for (int col = 0; col < 16; ++col) {
					const auto byte = mem_readb(PhysicalMake(
					        cs, static_cast<uint16_t>(off + col)));
					std::fprintf(file, " %02X", byte);
				}
				std::fprintf(file, "\n");
			}
		}
	}
	std::fclose(file);

	file = std::fopen("sea2_caller_at_read.txt", "wt");
	if (!file) {
		return;
	}
	for (size_t i = 0; i < stack_sample_total; ++i) {
		std::fprintf(file, "sample %u caller %04X:%04X\n",
		             static_cast<unsigned>(i), stack_samples[i].words[5],
		             stack_samples[i].words[4]);
		std::fprintf(file, "  ");
		for (const auto byte : caller_bytes[i]) {
			std::fprintf(file, "%02X ", byte);
		}
		std::fprintf(file, "\n");
	}
	std::fclose(file);

	file = std::fopen("sea2_int21_ring.txt", "wt");
	if (!file) {
		return;
	}
	for (size_t i = 0; i < int21_sample_total; ++i) {
		const auto& s = int21_samples[i];
		std::fprintf(file,
		             "sample %u ss=%04X sp=%04X bx=%04X cx=%04X dx=%04X "
		             "top=%04X next=%04X\n",
		             static_cast<unsigned>(i), s.ss, s.sp, s.bx, s.cx, s.dx,
		             mem_readw(PhysicalMake(
		                     s.ss, static_cast<uint16_t>(s.sp))),
		             mem_readw(PhysicalMake(
		                     s.ss, static_cast<uint16_t>(s.sp + 2))));
		for (const auto entry : s.history) {
			if (entry == 0) {
				continue;
			}
			std::fprintf(file, "  %04X:%04X\n",
			             static_cast<uint16_t>(entry >> 16),
			             static_cast<uint16_t>(entry & 0xffff));
		}
	}
	std::fclose(file);

	file = std::fopen("sea2_blitter_args.txt", "wt");
	if (!file) {
		return;
	}
	std::fprintf(file, "entries=%u samples=%u\n",
	             static_cast<unsigned>(blitter_entry_total),
	             static_cast<unsigned>(blitter_sample_total));
	for (size_t i = 0; i < blitter_sample_total; ++i) {
		const auto& s = blitter_samples[i];
		std::fprintf(file,
		             "%04X bp=%04X ax=%04X bx=%04X cx=%04X dx=%04X si=%04X "
		             "di=%04X es=%04X ds=%04X args=",
		             s.ip, s.bp, s.ax, s.bx, s.cx, s.dx, s.si, s.di, s.es,
		             s.ds);
		for (const auto a : s.args) {
			std::fprintf(file, "%04X ", a);
		}
		std::fprintf(file, "\nglyph:");
		for (const auto b : s.glyph) {
			std::fprintf(file, " %02X", b);
		}
		std::fprintf(file, "\n");
	}
	std::fclose(file);

	file = std::fopen("sea2_int16_sites.txt", "wt");
	if (!file) {
		return;
	}
	std::fprintf(file, "total=%u\ncs:ip count last_ah last_ax\n",
	             static_cast<unsigned>(key_site_total));
	{
		std::vector<std::pair<uint32_t, const KeySite*>> sites;
		for (const auto& entry : key_site_table) {
			if (entry.used && entry.count > 0) {
				sites.emplace_back(entry.count, &entry);
			}
		}
		std::sort(sites.begin(), sites.end(),
		          [](const auto& a, const auto& b) { return a.first > b.first; });
		for (const auto& s : sites) {
			std::fprintf(file, "%04X:%04X %u %02X %04X\n", s.second->cs,
			             s.second->ip, s.first, s.second->last_ah,
			             s.second->last_ax);
		}
	}
	std::fclose(file);

	file = std::fopen("sea2_dos_calls.txt", "wt");
	if (!file) {
		return;
	}
	std::fprintf(file, "total=%u\ncs:ip ah count\n",
	             static_cast<unsigned>(dos_call_total));
	{
		std::vector<std::pair<uint32_t, const DosCallSite*>> sites;
		for (const auto& entry : dos_call_table) {
			if (entry.used && entry.count > 0) {
				sites.emplace_back(entry.count, &entry);
			}
		}
		std::sort(sites.begin(), sites.end(),
		          [](const auto& a, const auto& b) { return a.first > b.first; });
		for (const auto& s : sites) {
			std::fprintf(file, "%04X:%04X %02X %u\n", s.second->cs,
			             s.second->ip, s.second->ah, s.first);
		}
	}
	std::fclose(file);

	file = std::fopen("sea2_hot_ip.txt", "wt");
	if (!file) {
		return;
	}
	std::fprintf(file, "samples=%u\ncs:ip count\n",
	             static_cast<unsigned>(hot_site_total));
	{
		std::vector<std::pair<uint32_t, const HotSite*>> sites;
		for (const auto& entry : hot_site_table) {
			if (entry.used && entry.count > 0) {
				sites.emplace_back(entry.count, &entry);
			}
		}
		std::sort(sites.begin(), sites.end(),
		          [](const auto& a, const auto& b) { return a.first > b.first; });
		const size_t limit = std::min<size_t>(sites.size(), 200);
		for (size_t i = 0; i < limit; ++i) {
			std::fprintf(file, "%04X:%04X %u\n", sites[i].second->cs,
			             sites[i].second->ip, sites[i].first);
		}
	}
	std::fclose(file);

	file = std::fopen("sea2_entry_stack.txt", "wt");
	if (!file) {
		return;
	}
	for (size_t i = 0; i < entry_sample_total; ++i) {
		const auto& s = entry_samples[i];
		std::fprintf(file, "%04X:%04X ss=%04X sp=%04X stack:", s.cs, s.ip,
		             s.ss, s.sp);
		for (const auto w : s.words) {
			std::fprintf(file, " %04X", w);
		}
		std::fprintf(file, "\n");
	}
	std::fclose(file);
}

uint32_t SEA2_HD_GlyphTraditional(const uint16_t record)
{
	if (record < sea2_glyph_map::FirstRecord) {
		return 0;
	}
	const uint32_t index = record - sea2_glyph_map::FirstRecord;
	if (index >= sea2_glyph_map::RecordCount) {
		return 0;
	}
	return sea2_glyph_map::kEntries[index].traditional;
}

uint32_t SEA2_HD_GlyphSimplified(const uint16_t record)
{
	if (record < sea2_glyph_map::FirstRecord) {
		return 0;
	}
	const uint32_t index = record - sea2_glyph_map::FirstRecord;
	if (index >= sea2_glyph_map::RecordCount) {
		return 0;
	}
	return sea2_glyph_map::kEntries[index].simplified;
}

void SEA2_HD_VerifyGlyphMap()
{
	struct Check {
		uint16_t record;
		uint32_t traditional;
		uint32_t simplified;
	};
	static constexpr Check checks[] = {
	        {684, 0x4E00, 0x4E00},   // 一
	        {1900, 0x5EA6, 0x5EA6},  // 度
	        {2407, 0x6D77, 0x6D77},  // 海
	        {2545, 0x822A, 0x822A},  // 航
	        {3551, 0x9032, 0x8FDB},  // 進 -> 进
	        {5770, 0x7E7C, 0x7EE7},  // 繼 -> 继
	        {5852, 0x7E8C, 0x7EED},  // 續 -> 续
	        {6084, 0x7C72, 0x5401},  // 籲 -> 吁
	};
	uint32_t mapped = 0;
	for (const auto& entry : sea2_glyph_map::kEntries) {
		if (entry.traditional != 0) {
			++mapped;
		}
	}
	bool ok = true;
	for (const auto& check : checks) {
		const auto traditional = SEA2_HD_GlyphTraditional(check.record);
		const auto simplified  = SEA2_HD_GlyphSimplified(check.record);
		if (traditional != check.traditional ||
		    simplified != check.simplified) {
			ok = false;
			LOG_MSG("SEA2HD: glyph map mismatch record=%u got U+%04X/U+%04X expected U+%04X/U+%04X",
			        check.record, static_cast<unsigned>(traditional),
			        static_cast<unsigned>(simplified),
			        static_cast<unsigned>(check.traditional),
			        static_cast<unsigned>(check.simplified));
		}
	}
	LOG_MSG("SEA2HD: glyph map %s; %u/%u records mapped",
	        ok ? "OK" : "BROKEN", static_cast<unsigned>(mapped),
	        static_cast<unsigned>(sea2_glyph_map::RecordCount));
}

void SEA2_HD_SetMode(const std::string& mode, const std::string& assets_path)
{
	const auto new_hd_enabled = iequals(mode, "on");
	if (hd_enabled != new_hd_enabled) {
		draw_commands.clear();
		frame_dirty = true;
	}
	hd_enabled = new_hd_enabled;
	trace_enabled = iequals(mode, "trace");
	sea2_hd_string_trace = trace_enabled;

	const std::filesystem::path new_root = assets_path.empty() ? "../assets/hd"
	                                                           : assets_path;
	if (assets_root != new_root) {
		assets_root = new_root;
		reset_assets();
		frame_dirty = true;
	}

	if (!hd_enabled && !trace_enabled) {
		SEA2_HD_ClearProgram();
	}
}

bool SEA2_HD_IsActive()
{
	return hd_enabled && sea2_hd_trace_loadseg != 0;
}

bool SEA2_HD_NeedsFrame()
{
	if (!hd_enabled) {
		return false;
	}
	if (frame_dirty) {
		return true;
	}
	// A box that is still settling needs the next frame to be composited even
	// when the game itself draws nothing, otherwise it could stay unlocked (and
	// therefore low resolution) as long as the screen does not change.
	for (const auto& command : draw_commands) {
		if (!command.locked && command.unsettled < KaoSettleGiveUpFrames) {
			return true;
		}
	}
	return false;
}

void SEA2_HD_AdvanceFrame()
{
	blit_seq_flush_frame();
	++blit_seq_frames;
	if (sea2_hd_string_trace) {
		if (++string_op_frames >= StringOpDumpInterval) {
			string_op_frames = 0;
			SEA2_HD_DumpStringTrace();
		}
	}
}

void SEA2_HD_SetFrameDump(const int interval)
{
	sea2_hd_frame_dump_interval = std::max(interval, 0);
	sea2_hd_frame_dumps         = 0;
	if (sea2_hd_frame_dump_interval > 0) {
		LOG_MSG("SEA2HD: dumping every %dth frame into sea2_frames/",
		        sea2_hd_frame_dump_interval);
	}
}

// The frame is handed over in the very same 32-bit BGRX layout that is
// composited, so the dump shows exactly what the game put on screen.
void SEA2_HD_DumpFrame(const void* pixels, const int pitch, const int width,
                       const int height)
{
	if (sea2_hd_frame_dump_interval <= 0 || !pixels || width != VgaWidth ||
	    height != VgaHeight || sea2_hd_frame_dumps >= FrameDumpMax) {
		return;
	}

	const auto frame = blit_seq_frames;
	if (frame % static_cast<uint32_t>(sea2_hd_frame_dump_interval) != 0) {
		return;
	}
	++sea2_hd_frame_dumps;

	auto* surface = SDL_CreateRGBSurfaceWithFormat(
	        0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
	if (!surface) {
		return;
	}

	if (SDL_LockSurface(surface) == 0) {
		for (int y = 0; y < height; ++y) {
			auto* out = static_cast<uint8_t*>(surface->pixels) +
			            static_cast<size_t>(y) * surface->pitch;
			const auto* in = static_cast<const uint8_t*>(pixels) +
			                 static_cast<size_t>(y) * pitch;
			for (int x = 0; x < width; ++x) {
				out[x * 4 + 0] = in[x * 4 + 2];
				out[x * 4 + 1] = in[x * 4 + 1];
				out[x * 4 + 2] = in[x * 4 + 0];
				out[x * 4 + 3] = 0xff;
			}
		}
		SDL_UnlockSurface(surface);

		std::filesystem::create_directories("sea2_frames");
		char name[64] = {};
		std::snprintf(name, sizeof(name), "sea2_frames/f_%05u.png",
		              static_cast<unsigned>(frame));
		IMG_SavePNG(surface, name);
	}
	SDL_FreeSurface(surface);
}

// The port is drawn with a 16 colour palette that the game swaps as the day
// passes. Those colours come out of the VGA's six bit per channel DAC, so the two
// low bits are rounding only and are dropped here: a colour of the reference map
// and the same colour in the frame then compare equal whichever time of day
// produced them.
uint32_t port_colour(const uint32_t rgb)
{
	return (((rgb >> 18) & 0x3f) << 12) | (((rgb >> 10) & 0x3f) << 6) |
	       ((rgb >> 2) & 0x3f);
}

// One hash per 16x16 cell. Both the map and the window are grid aligned, so a
// cell either is exactly that tile or it is not, and a window full of them pins
// the offset to the tile without any tolerance to tune.
uint64_t hash_port_cell(const uint32_t* pixels, const int stride)
{
	uint64_t hash = 0xcbf29ce484222325ull;
	for (int y = 0; y < PortMapStep; ++y) {
		const auto* row = pixels + static_cast<size_t>(y) * stride;
		for (int x = 0; x < PortMapStep; ++x) {
			hash = (hash ^ port_colour(row[x])) * 0x100000001b3ull;
		}
	}
	return hash;
}

void hash_port_window(const uint32_t* source, const int source_width,
                      std::array<uint64_t, PortMapCellCount>& window)
{
	for (int ty = 0; ty < PortMapCells; ++ty) {
		for (int tx = 0; tx < PortMapCells; ++tx) {
			window[ty * PortMapCells + tx] = hash_port_cell(
			        source +
			                static_cast<size_t>(PortMapWindowY + ty * PortMapStep) *
			                        source_width +
			                PortMapWindowX + tx * PortMapStep,
			        source_width);
		}
	}
}

// The same cell, described by which of its neighbouring pixels are the same
// colour instead of by the colours themselves. A palette is a function of the
// colour index, so a fade can only turn a pair of different pixels into an equal
// one: every bit that is set here stays set while the screen fades in, which is
// what makes the cell recognisable before its palette has arrived.
void port_cell_relations(const uint32_t* pixels, const int stride,
                         std::array<uint64_t, PortMapRelationWords>& relations)
{
	relations.fill(0);
	const auto set_equal = [&relations](const int bit, const uint32_t a,
	                                    const uint32_t b) {
		if ((a & 0x00ffffffu) == (b & 0x00ffffffu)) {
			relations[static_cast<size_t>(bit) / 64] |= 1ull << (bit % 64);
		}
	};
	for (int y = 0; y < PortMapStep; ++y) {
		const auto* row = pixels + static_cast<size_t>(y) * stride;
		for (int x = 0; x < PortMapStep - 1; ++x) {
			set_equal(y * (PortMapStep - 1) + x, row[x], row[x + 1]);
		}
	}
	for (int y = 0; y < PortMapStep - 1; ++y) {
		const auto* row   = pixels + static_cast<size_t>(y) * stride;
		const auto* below = row + stride;
		for (int x = 0; x < PortMapStep; ++x) {
			set_equal(PortMapRightBits + y * PortMapStep + x, row[x], below[x]);
		}
	}
}

uint64_t hash_port_relations(
        const std::array<uint64_t, PortMapRelationWords>& relations)
{
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const auto word : relations) {
		hash = (hash ^ word) * 0x100000001b3ull;
	}
	return hash;
}

void hash_port_window_relations(const uint32_t* source, const int source_width,
                                std::array<uint64_t, PortMapCellCount>& window)
{
	std::array<uint64_t, PortMapRelationWords> relations = {};
	for (int ty = 0; ty < PortMapCells; ++ty) {
		for (int tx = 0; tx < PortMapCells; ++tx) {
			port_cell_relations(
			        source +
			                static_cast<size_t>(PortMapWindowY + ty * PortMapStep) *
			                        source_width +
			                PortMapWindowX + tx * PortMapStep,
			        source_width, relations);
			window[ty * PortMapCells + tx] = hash_port_relations(relations);
		}
	}
}

// How many cells of the window show the very tile the map has at that offset.
int score_port_offset(
        const std::array<uint64_t, PortMapGridSize * PortMapGridSize>& cells,
        const int step_x, const int step_y,
        const std::array<uint64_t, PortMapCellCount>& window)
{
	int same = 0;
	for (int ty = 0; ty < PortMapCells; ++ty) {
		const auto* row = cells.data() +
		                  static_cast<size_t>(step_y + ty) * PortMapGridSize +
		                  step_x;
		for (int tx = 0; tx < PortMapCells; ++tx) {
			same += row[tx] == window[ty * PortMapCells + tx] ? 1 : 0;
		}
	}
	return same;
}

// The same score, but the cells the game painted a panel over are left out: a
// message box can hide more than half of the port, and the part that is still
// the map has to decide on its own. Panels cover a large flat area, so they are
// found without knowing where the game put the box - see port_panel_mask().
int score_port_offset_with_panels(
        const std::array<uint64_t, PortMapGridSize * PortMapGridSize>& cells,
        const int step_x, const int step_y,
        const std::array<uint64_t, PortMapCellCount>& window,
        const uint8_t* panel, int& panels_out)
{
	int same   = 0;
	int panels = 0;
	for (int ty = 0; ty < PortMapCells; ++ty) {
		const auto* row = cells.data() +
		                  static_cast<size_t>(step_y + ty) * PortMapGridSize +
		                  step_x;
		for (int tx = 0; tx < PortMapCells; ++tx) {
			int paper = 0;
			for (int y = 0; y < PortMapStep; ++y) {
				const auto* line = panel +
				                   static_cast<size_t>(ty * PortMapStep + y) *
				                           PortMapWindowSize +
				                   tx * PortMapStep;
				for (int x = 0; x < PortMapStep; ++x) {
					paper += (line[x] & 3) == 3 ? 1 : 0;
				}
			}
			if (paper >= PortMapPanelCellPixels) {
				++panels;
				continue;
			}
			same += row[tx] == window[ty * PortMapCells + tx] ? 1 : 0;
		}
	}
	panels_out = panels;
	return same;
}

// The cell whose picture repeats in the fewest places pins the offset; every
// place that picture appears is tried and the whole window decides.
bool locate_port_map(
        const std::array<uint64_t, PortMapGridSize * PortMapGridSize>& cells,
        const std::unordered_map<uint64_t, std::vector<uint16_t>>& index,
        const int min_cells,
        const std::array<uint64_t, PortMapCellCount>& window,
        const int keep_step_x, const int keep_step_y, int& step_x, int& step_y,
        int& best_score_out, int& known_out)
{
	auto anchor         = -1;
	auto anchor_repeats = PortMapAnchorMax + 1;
	known_out           = 0;
	for (int i = 0; i < PortMapCellCount; ++i) {
		const auto it = index.find(window[static_cast<size_t>(i)]);
		if (it == index.end()) {
			continue;
		}
		++known_out;
		if (it->second.size() < anchor_repeats) {
			anchor         = i;
			anchor_repeats = it->second.size();
			if (anchor_repeats == 1) {
				break;
			}
		}
	}
	if (anchor < 0) {
		best_score_out = -1;
		return false;
	}

	const auto anchor_step_x = anchor % PortMapCells;
	const auto anchor_step_y = anchor / PortMapCells;
	int best_score           = -1;
	int best_x               = 0;
	int best_y               = 0;
	const auto& places =
	        index.find(window[static_cast<size_t>(anchor)])->second;
	for (const auto cell : places) {
		const auto x = static_cast<int>(cell) % PortMapGridSize - anchor_step_x;
		const auto y = static_cast<int>(cell) / PortMapGridSize - anchor_step_y;
		if (x < 0 || y < 0 || x > PortMapOffsetSteps || y > PortMapOffsetSteps) {
			continue;
		}
		const auto score = score_port_offset(cells, x, y, window);
		const auto keeps = x == keep_step_x && y == keep_step_y;
		// A picture that repeats leaves several offsets equally good; staying
		// where it was keeps the artwork from jumping between them.
		if (score > best_score || (score == best_score && keeps)) {
			best_score = score;
			best_x     = x;
			best_y     = y;
		}
	}
	best_score_out = best_score;
	if (best_score < min_cells) {
		return false;
	}
	step_x = best_x;
	step_y = best_y;
	return true;
}

// Writes what the game shows and what was composited over it for the very same
// frame, so a region that stays low resolution can be told apart from one that
// was drawn at the wrong place.
void diag_port_cells(const uint32_t* source, const int source_width,
                     const uint32_t* destination, const int destination_stride,
                     const char* states)
{
	const auto started = port_map_diags++;
	diag("PORTCELLS frame=%u off=%d,%d", composite_count, port_map_offset_x,
	     port_map_offset_y);
	for (int ty = 0; ty < PortMapCells; ++ty) {
		char row_text[PortMapCells + 1] = {};
		std::memcpy(row_text, states + ty * PortMapCells, PortMapCells);
		diag("PORTCELLS %s", row_text);
	}

	std::filesystem::create_directories("sea2_portcell");
	char name[96] = {};
	std::snprintf(name, sizeof(name), "sea2_portcell/f%05u_src.png", started);
	auto* src = SDL_CreateRGBSurfaceWithFormat(0, PortMapWindowSize,
	                                          PortMapWindowSize, 32,
	                                          SDL_PIXELFORMAT_RGBA32);
	if (src && SDL_LockSurface(src) == 0) {
		for (int y = 0; y < PortMapWindowSize; ++y) {
			auto* out = static_cast<uint8_t*>(src->pixels) +
			            static_cast<size_t>(y) * src->pitch;
			const auto* in = source +
			                 static_cast<size_t>(PortMapWindowY + y) *
			                         source_width +
			                 PortMapWindowX;
			for (int x = 0; x < PortMapWindowSize; ++x) {
				out[x * 4 + 0] = static_cast<uint8_t>((in[x] >> 16) & 0xff);
				out[x * 4 + 1] = static_cast<uint8_t>((in[x] >> 8) & 0xff);
				out[x * 4 + 2] = static_cast<uint8_t>(in[x] & 0xff);
				out[x * 4 + 3] = 0xff;
			}
		}
		SDL_UnlockSurface(src);
		IMG_SavePNG(src, name);
	}
	if (src) {
		SDL_FreeSurface(src);
	}

	std::snprintf(name, sizeof(name), "sea2_portcell/f%05u_dst.png", started);
	const auto size = PortMapWindowSize * HdScale;
	auto* dst = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32,
	                                          SDL_PIXELFORMAT_RGBA32);
	if (dst && SDL_LockSurface(dst) == 0) {
		for (int y = 0; y < size; ++y) {
			auto* out = static_cast<uint8_t*>(dst->pixels) +
			            static_cast<size_t>(y) * dst->pitch;
			const auto* in = destination +
			                 static_cast<size_t>(PortMapWindowY * HdScale + y) *
			                         destination_stride +
			                 PortMapWindowX * HdScale;
			for (int x = 0; x < size; ++x) {
				out[x * 4 + 0] = static_cast<uint8_t>((in[x] >> 16) & 0xff);
				out[x * 4 + 1] = static_cast<uint8_t>((in[x] >> 8) & 0xff);
				out[x * 4 + 2] = static_cast<uint8_t>(in[x] & 0xff);
				out[x * 4 + 3] = 0xff;
			}
		}
		SDL_UnlockSurface(dst);
		IMG_SavePNG(dst, name);
	}
	if (dst) {
		SDL_FreeSurface(dst);
	}
}

// How much of the window is black, sampled. A screen the game is still painting
// is mostly black, one that is simply something else is not.
int dark_pixels(const uint32_t* source, const int source_width)
{
	int dark = 0;
	for (int y = 0; y < PortMapWindowSize; y += 8) {
		const auto* row = source +
		                  static_cast<size_t>(PortMapWindowY + y) * source_width +
		                  PortMapWindowX;
		for (int x = 0; x < PortMapWindowSize; x += 8) {
			const auto colour = row[x];
			dark += ((colour >> 16) & 0xff) + ((colour >> 8) & 0xff) +
			                (colour & 0xff) <
			        30;
		}
	}
	return dark;
}

// Marks the pixels of the window the game painted a panel over. Only a pixel
// that sits in a long flat run across *and* down is paper: every one of the
// map's tiles is dithered, so the map never has such an area, while a message
// box, a label or a menu is one colour over a large area.
void port_panel_mask(const uint32_t* source, const int source_width,
                     uint8_t* panel)
{
	std::memset(panel, 0,
	            static_cast<size_t>(PortMapWindowSize) * PortMapWindowSize);
	for (int y = 0; y < PortMapWindowSize; ++y) {
		const auto* row = source +
		                  static_cast<size_t>(PortMapWindowY + y) * source_width +
		                  PortMapWindowX;
		for (int x = 0; x < PortMapWindowSize;) {
			const auto colour = row[x] & 0x00ffffffu;
			auto end          = x + 1;
			while (end < PortMapWindowSize &&
			       (row[end] & 0x00ffffffu) == colour) {
				++end;
			}
			if (end - x >= PortMapPanelRun) {
				for (auto i = x; i < end; ++i) {
					panel[static_cast<size_t>(y) * PortMapWindowSize + i] |= 1;
				}
			}
			x = end;
		}
	}
	for (int x = 0; x < PortMapWindowSize; ++x) {
		const auto column = static_cast<size_t>(PortMapWindowX + x);
		for (int y = 0; y < PortMapWindowSize;) {
			const auto colour =
			        source[static_cast<size_t>(PortMapWindowY + y) * source_width +
			               column] &
			        0x00ffffffu;
			auto end = y + 1;
			while (end < PortMapWindowSize &&
			       (source[static_cast<size_t>(PortMapWindowY + end) *
			                       source_width +
			               column] &
			        0x00ffffffu) == colour) {
				++end;
			}
			if (end - y >= PortMapPanelRun) {
				for (auto i = y; i < end; ++i) {
					panel[static_cast<size_t>(i) * PortMapWindowSize + x] |= 2;
				}
			}
			y = end;
		}
	}
}

// Saves the 384x384 map window of a frame, used to look at the frames in which
// the map was not replaced.
void dump_port_window(const uint32_t* source, const int source_width,
                      const char* path)
{
	auto* surface = SDL_CreateRGBSurfaceWithFormat(0, PortMapWindowSize,
	                                              PortMapWindowSize, 32,
	                                              SDL_PIXELFORMAT_RGBA32);
	if (!surface) {
		return;
	}
	if (SDL_LockSurface(surface) == 0) {
		for (int y = 0; y < PortMapWindowSize; ++y) {
			auto* out = static_cast<uint8_t*>(surface->pixels) +
			            static_cast<size_t>(y) * surface->pitch;
			const auto* in = source +
			                 static_cast<size_t>(PortMapWindowY + y) *
			                         source_width +
			                 PortMapWindowX;
			for (int x = 0; x < PortMapWindowSize; ++x) {
				out[x * 4 + 0] = static_cast<uint8_t>((in[x] >> 16) & 0xff);
				out[x * 4 + 1] = static_cast<uint8_t>((in[x] >> 8) & 0xff);
				out[x * 4 + 2] = static_cast<uint8_t>(in[x] & 0xff);
				out[x * 4 + 3] = 0xff;
			}
		}
		SDL_UnlockSurface(surface);
		IMG_SavePNG(surface, path);
	}
	SDL_FreeSurface(surface);
}

// The game fades its screen out whenever it leaves the map, so for a few frames
// the window is a darker copy of it rather than an exact one. Such a window is
// still the map, and the fade is a darkening of the palette - often with every
// other pixel already black - so the pixels that are black carry no information
// about the scale and are left out. One brightness per channel is fitted on the
// rest.
bool port_map_fit_gain(const PortMap& map, const int step_x, const int step_y,
                       const uint32_t* source, const int source_width,
                       std::array<float, 3>& scale)
{
	const auto lit = [](const uint32_t colour) {
		return ((colour >> 16) & 0xff) + ((colour >> 8) & 0xff) +
		               (colour & 0xff) >=
		       30;
	};

	double source_sum[3]   = {0.0, 0.0, 0.0};
	double expected_sum[3] = {0.0, 0.0, 0.0};
	int fitted             = 0;
	for (int y = 0; y < PortMapWindowSize; y += 4) {
		const auto* row = source +
		                  static_cast<size_t>(PortMapWindowY + y) * source_width +
		                  PortMapWindowX;
		const auto* expected = map.pixels.data() +
		                       static_cast<size_t>(step_y * PortMapStep + y) *
		                               PortMapSize +
		                       step_x * PortMapStep;
		for (int x = 0; x < PortMapWindowSize; x += 4) {
			const auto actual = row[x];
			const auto want   = expected[x];
			if (!lit(actual) || !lit(want)) {
				continue;
			}
			source_sum[0] += (actual >> 16) & 0xff;
			source_sum[1] += (actual >> 8) & 0xff;
			source_sum[2] += actual & 0xff;
			expected_sum[0] += (want >> 16) & 0xff;
			expected_sum[1] += (want >> 8) & 0xff;
			expected_sum[2] += want & 0xff;
			++fitted;
		}
	}
	if (fitted < 60) {
		return false;
	}

	for (auto i = 0; i < 3; ++i) {
		const auto channel = static_cast<size_t>(i);
		if (expected_sum[channel] < 1.0) {
			return false;
		}
		const auto ratio = static_cast<float>(source_sum[channel] /
		                                      expected_sum[channel]);
		if (ratio < 0.10f || ratio > 4.0f) {
			return false;
		}
		scale[channel] = ratio;
	}
	return true;
}

// The game paints the whole port with one palette, so each of the handful of
// colours the map is made of turns into exactly one colour on screen. Which one
// is read off the window itself: within the cells whose equality pattern is the
// map's - they cannot hold the character or a dialog - the screen colour that
// owns a map colour is the one the palette produced from it. That mapping is
// exact whatever the palette is, so no fade and no time of day between the
// stored ones has to be guessed at.
//
// One stored colour can be sent to two screen colours: the four times of day
// were read off screenshots, and two entries of the game's palette that looked
// alike there are told apart again by a palette that fades in between. The map
// dithers those two entries in an even pattern, so whenever the split is even
// both halves are adopted; a dialog or the character instead leaves one large
// and one small part, and such a colour is still refused.
//
// The cells are only consulted when the window's pattern is the map's. A
// trusted offset - the frame before drew the port at it - skips that test and
// reads the palette off every pixel of the window instead, because a palette
// that fades to black makes neighbouring colours equal and breaks the pattern
// while the port is still there.
bool measure_port_palette(const PortMap& map, const int step_x, const int step_y,
                          const std::array<uint64_t, PortMapCellCount>& window,
                          const uint32_t* source, const int source_width,
                          std::vector<std::pair<uint32_t, uint32_t>>& pairs,
                          const bool whole_window = false)
{
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, int>> seen = {};
	const auto sample_step                                               = 4;
	const auto* panel = port_map_panel_ready ? port_map_panel.data() : nullptr;
	for (int ty = 0; ty < PortMapCells; ++ty) {
		for (int tx = 0; tx < PortMapCells; ++tx) {
			const auto cell = (step_y + ty) * PortMapGridSize + step_x + tx;
			if (!whole_window &&
			    window[ty * PortMapCells + tx] !=
			            map.relation_hashes[static_cast<size_t>(cell)]) {
				continue;
			}
			const auto source_x = PortMapWindowX + tx * PortMapStep;
			const auto source_y = PortMapWindowY + ty * PortMapStep;
			const auto* map_x0 =
			        map.pixels.data() +
			        static_cast<size_t>(step_y * PortMapStep + ty * PortMapStep) *
			                        PortMapSize +
			        (step_x + tx) * PortMapStep;
			for (int y = 0; y < PortMapStep; y += sample_step) {
				const auto* row = source +
				                  static_cast<size_t>(source_y + y) *
				                          source_width +
				                  source_x;
				const auto* expected =
				        map_x0 + static_cast<size_t>(y) * PortMapSize;
				const auto* mask = panel == nullptr
				                           ? nullptr
				                           : panel +
				                                     static_cast<size_t>(
				                                             ty * PortMapStep + y) *
				                                             PortMapWindowSize +
				                                     tx * PortMapStep;
				for (int x = 0; x < PortMapStep; x += sample_step) {
					if (mask != nullptr && (mask[x] & 3) == 3) {
						continue;
					}
					++seen[expected[x] & 0x00ffffffu][row[x] & 0x00ffffffu];
				}
			}
		}
	}

	// The mapping is only replaced by a frame that really produced one. A single
	// unreadable frame would otherwise drop the drawing back onto the other path
	// and the port would change its brightness for that frame, which is exactly
	// what flickers while the character walks.
	std::vector<std::pair<uint32_t, uint32_t>> measured = {};
	auto sampled                                        = 0;
	auto explained                                      = 0;
	for (const auto& [colour, screens] : seen) {
		auto total       = 0;
		auto best        = 0;
		auto best_colour = 0u;
		auto next        = 0;
		auto next_colour = 0u;
		for (const auto& [screen, count] : screens) {
			total += count;
			if (count > best) {
				next        = best;
				next_colour = best_colour;
				best        = count;
				best_colour = screen;
			} else if (count > next) {
				next        = count;
				next_colour = screen;
			}
		}
		sampled += total;
		// A colour mostly given to something else is not the palette's doing.
		if (best * 4 >= total * 3) {
			measured.emplace_back(colour, best_colour);
			explained += best;
			continue;
		}
		// Two entries of the map's palette that share this colour in the stored
		// picture, told apart again by the palette of this frame.
		if (next * 100 >= total * 30 && (best + next) * 100 >= total * 90) {
			measured.emplace_back(colour, best_colour);
			measured.emplace_back(colour, next_colour);
			explained += best + next;
		}
	}
	if (measured.empty()) {
		return false;
	}
	// A window that was not checked cell by cell has to be explained by the
	// mapping almost completely, or it is not the port at that offset.
	if (whole_window) {
		if (explained * 100 < sampled * PortMapBridgeExplainedPercent) {
			return false;
		}
		// A screen that has already gone black holds a single colour, so nothing
		// of the map can be told apart any more and drawing there would invent
		// detail the game does not show.
		auto distinct = std::vector<uint32_t>{};
		for (const auto& [colour, screen] : measured) {
			static_cast<void>(colour);
			if (std::find(distinct.begin(), distinct.end(), screen) ==
			    distinct.end()) {
				distinct.push_back(screen);
			}
		}
		if (distinct.size() < PortMapBridgeMinColours) {
			return false;
		}
	}
	std::sort(measured.begin(), measured.end());
	pairs = std::move(measured);
	return true;
}

// The fitted brightness is only trusted when almost every pixel agrees with it.
bool port_map_scaled(const PortMap& map, const int step_x, const int step_y,
                     const uint32_t* source, const int source_width,
                     std::array<float, 3>& scale)
{
	const auto lit = [](const uint32_t colour) {
		return ((colour >> 16) & 0xff) + ((colour >> 8) & 0xff) +
		               (colour & 0xff) >=
		       30;
	};

	if (!port_map_fit_gain(map, step_x, step_y, source, source_width, scale)) {
		return false;
	}

	int agree  = 0;
	int looked = 0;
	for (int y = 0; y < PortMapWindowSize; y += 4) {
		const auto* row = source +
		                  static_cast<size_t>(PortMapWindowY + y) * source_width +
		                  PortMapWindowX;
		const auto* expected = map.pixels.data() +
		                       static_cast<size_t>(step_y * PortMapStep + y) *
		                               PortMapSize +
		                       step_x * PortMapStep;
		for (int x = 0; x < PortMapWindowSize; x += 4) {
			const auto actual = row[x];
			const auto want   = expected[x];
			if (!lit(actual) || !lit(want)) {
				continue;
			}
			const auto dr = static_cast<int>((actual >> 16) & 0xff) -
			                std::min(255,
			                         static_cast<int>(((want >> 16) & 0xff) *
			                                          scale[0]));
			const auto dg = static_cast<int>((actual >> 8) & 0xff) -
			                std::min(255,
			                         static_cast<int>(((want >> 8) & 0xff) *
			                                          scale[1]));
			const auto db = static_cast<int>(actual & 0xff) -
			                std::min(255,
			                         static_cast<int>((want & 0xff) * scale[2]));
			++looked;
			if (dr >= -14 && dr <= 14 && dg >= -14 && dg <= 14 && db >= -14 &&
			    db <= 14) {
				++agree;
			}
		}
	}
	return looked >= 60 && agree * 100 >= looked * 90;
}

// Decides for this frame whether the window shows a map, and where. The map and
// its offset are remembered while something else is on screen (a building, a
// shop, a dialog), so walking back out needs no search at all.
void update_port_map(const uint32_t* source, const int source_width)
{
	if (port_maps.empty()) {
		return;
	}

	std::array<uint64_t, PortMapCellCount> window = {};
	hash_port_window(source, source_width, window);

	const auto selected = port_map_selected;
	const auto keep_x   = port_map_offset_x / PortMapStep;
	const auto keep_y   = port_map_offset_y / PortMapStep;

	port_map_visible  = false;
	port_map_fade     = {1.0f, 1.0f, 1.0f};
	port_map_measured = false;
	port_map_panel_ready = false;
	std::array<uint64_t, PortMapCellCount> relation_window = {};
	bool relation_window_ready                              = false;
	const auto relation_window_of =
	        [&]() -> const std::array<uint64_t, PortMapCellCount>& {
		if (!relation_window_ready) {
			hash_port_window_relations(source, source_width, relation_window);
			relation_window_ready = true;
		}
		return relation_window;
	};
	int keep_score      = -1;
	int relation_keep   = -1;
	bool scaled_ok      = false;
	std::array<float, 3> scale = {};
	if (selected >= 0) {
		const auto& map = port_maps[static_cast<size_t>(selected)];
		keep_score      = score_port_offset(map.cells, keep_x, keep_y, window);
		if (keep_score >= PortMapKeepCells) {
			// The window is the map and its palette is the stored one, but the
			// same drawing is used here as for every other palette: the artwork
			// keeps the colour the game shows and only adds its own detail, so
			// the port neither brightens nor changes its look between frames.
			port_map_visible  = true;
			port_map_measured = measure_port_palette(
			                           map, keep_x, keep_y,
			                           relation_window_of(), source, source_width,
			                           port_map_palette_pairs) ||
			                    !port_map_palette_pairs.empty();
			return;
		}

		// A fade out keeps the very same map on screen, only darker, so the
		// artwork is dimmed with it instead of handing the window back to the
		// game's own pixels until the fade is over.
		scaled_ok = port_map_scaled(map, keep_x, keep_y, source, source_width,
		                            scale);

		if (scaled_ok) {
			// A fade is a palette that got darker entry by entry, so the same
			// measurement describes it exactly; the fitted brightness is only
			// the fallback for a frame it cannot describe.
			port_map_visible  = true;
			port_map_fade     = scale;
			port_map_measured = measure_port_palette(
			                           map, keep_x, keep_y,
			                           relation_window_of(), source, source_width,
			                           port_map_palette_pairs) ||
			                    !port_map_palette_pairs.empty();
			return;
		}

		// The palette is neither the stored one nor a darkening of it: the game
		// fades its palette in entry by entry, and between the stored times of day
		// it blends them. The picture is complete all the same, so the window is
		// checked once more by the equality pattern of its cells, which such a
		// palette cannot change.
		const auto relation_keep_score = score_port_offset(
		        map.relation_hashes, keep_x, keep_y, relation_window_of());
		relation_keep = relation_keep_score;
		if (relation_keep_score >= PortMapKeepCells) {
			port_map_visible  = true;
			port_map_measured = measure_port_palette(
			                           map, keep_x, keep_y,
			                           relation_window_of(), source, source_width,
			                           port_map_palette_pairs) ||
			                    !port_map_palette_pairs.empty();
			if (port_map_rel_logs < 40) {
				++port_map_rel_logs;
				diag("PORTREL keep x=%d y=%d score=%d colours=%zu frame=%u",
				     port_map_offset_x, port_map_offset_y, relation_keep_score,
				     port_map_palette_pairs.size(), composite_count);
				if (port_map_rel_logs <= 8) {
					std::string line = {};
					for (const auto& [colour, screen] :
					     port_map_palette_pairs) {
						char item[32] = {};
						std::snprintf(item, sizeof(item), " %06x>%06x", colour,
						              screen);
						line += item;
					}
					diag("PORTRELMAP%s", line.c_str());
				}
			}
			return;
		}

		// A message box the game opens over the port covers a large flat area,
		// and then neither the colours nor the equality pattern of the whole
		// window describe the map any more, even though the port has not moved.
		// The paper is recognised and left out, and the part of the window that
		// is still the map decides on its own - with the very colours of this
		// frame, so nothing is drawn brighter or darker than the game shows it.
		port_panel_mask(source, source_width, port_map_panel.data());
		port_map_panel_ready = true;
		int panels = 0;
		auto visible_cells = score_port_offset_with_panels(
		        map.cells, keep_x, keep_y, window, port_map_panel.data(),
		        panels);
		auto panels_by_relations = false;
		if (panels >= PortMapPanelMinCells && panels <= PortMapPanelMaxCells) {
			const auto rest  = PortMapCellCount - panels;
			const auto needs = (rest * PortMapPanelKeepPercent) / 100;
			if (visible_cells < needs) {
				visible_cells = score_port_offset_with_panels(
				        map.relation_hashes, keep_x, keep_y,
				        relation_window_of(), port_map_panel.data(), panels);
				panels_by_relations = true;
			}
			// Drawing a frame that a box partly covers is only safe with the
			// colours read off this very frame: the mapping of the frame before
			// can be brighter than the screen and would show as a flash.
			if (visible_cells >= needs &&
			    measure_port_palette(map, keep_x, keep_y, relation_window_of(),
			                         source, source_width,
			                         port_map_palette_pairs)) {
				port_map_visible  = true;
				port_map_measured = true;
				if (port_map_rel_logs < 40) {
					++port_map_rel_logs;
					diag("PORTPANEL keep x=%d y=%d panels=%d score=%d rest=%d rel=%d frame=%u",
					     port_map_offset_x, port_map_offset_y, panels,
					     visible_cells, rest, panels_by_relations ? 1 : 0,
					     composite_count);
				}
				return;
			}
		}
		// The game fades its whole palette out on its way into a building, and on
		// the way to black the entries collapse: colours the stored map keeps
		// apart become one, the equality pattern of the cells changes and the
		// port is no longer recognised although it is plainly still on screen.
		// It cannot move while the palette fades, so the offset of the frame
		// before is kept and the palette is read off the whole window - a screen
		// that already went all the way to black is refused by that measurement
		// itself, because it holds a single colour and no detail of the port can
		// be told apart in it. A building or a shop does not share the pattern at
		// all and is still left alone.
		if (port_map_drew_last && relation_keep >= PortMapBridgeMinCells &&
		    measure_port_palette(map, keep_x, keep_y, relation_window_of(),
		                         source, source_width, port_map_palette_pairs,
		                         true)) {
			port_map_visible  = true;
			port_map_measured = true;
			if (port_map_fade_logs < PortMapDiagCount) {
				++port_map_fade_logs;
				diag("PORTFADE keep x=%d y=%d rel=%d colours=%zu dark=%d frame=%u",
				     port_map_offset_x, port_map_offset_y, relation_keep,
				     port_map_palette_pairs.size(),
				     dark_pixels(source, source_width), composite_count);
			}
			return;
		}
	}

	if (port_map_retry > 0) {
		--port_map_retry;
		return;
	}

	int best_any  = -1;
	int known_any = 0;
	for (auto i = 0; i < static_cast<int>(port_maps.size()); ++i) {
		const auto& map   = port_maps[static_cast<size_t>(i)];
		const auto repeat = i == selected;
		int step_x        = 0;
		int step_y        = 0;
		int best          = -1;
		int known         = 0;
		auto found        = locate_port_map(
		        map.cells, map.cell_index, PortMapMinCells, window,
		        repeat ? keep_x : -1, repeat ? keep_y : -1, step_x, step_y, best,
		        known);

		// The colours of a window that is still fading in match nothing, but the
		// equality pattern of its cells already is the map's, so the same search is
		// run once more on that pattern before the frame is given up on.
		auto relations = false;
		if (!found) {
			found = locate_port_map(
			        map.relation_hashes, map.relation_index,
			        PortMapRelationMinCells, relation_window_of(),
			        repeat ? keep_x : -1, repeat ? keep_y : -1, step_x, step_y,
			        best, known);
			relations = found;
		}
		if (!found) {
			if (best > best_any) {
				best_any  = best;
				known_any = known;
			}
			continue;
		}
		if (port_map_selected != i) {
			// Another port: a mapping read off the previous one says nothing
			// about the colours of this one.
			port_map_palette_pairs.clear();
		}
		port_map_selected  = i;
		port_map_offset_x  = step_x * PortMapStep;
		port_map_offset_y  = step_y * PortMapStep;
		port_map_visible   = true;
		// The window is drawn the same way whichever test found it: the palette
		// is read off the frame itself, so the port keeps exactly one brightness
		// and cannot flash when the search and the kept offset take turns.
		port_map_measured = measure_port_palette(
		                           map, step_x, step_y, relation_window_of(),
		                           source, source_width,
		                           port_map_palette_pairs) ||
		                    !port_map_palette_pairs.empty();
		if (relations) {
			// The colours of the window are not the stored map's, because the game
			// either fades its palette in or uses a time of day between the stored
			// ones. The palette is therefore read off the frame itself, and the
			// artwork is drawn with those very colours.
			if (port_map_rel_logs < 40) {
				++port_map_rel_logs;
				diag("PORTREL entry=%u x=%d y=%d score=%d colours=%zu frame=%u",
				     map.index, port_map_offset_x, port_map_offset_y, best,
				     port_map_palette_pairs.size(), composite_count);
			}
		} else if (repeat) {
			diag("PORTMAP scroll %d,%d -> %d,%d frame=%u", keep_x * PortMapStep,
			     keep_y * PortMapStep, port_map_offset_x, port_map_offset_y,
			     composite_count);
		} else {
			LOG_MSG("SEA2HD: Port map %u at tile %d,%d", map.index, step_x,
			        step_y);
			diag("PORTMAP entry=%u x=%d y=%d frame=%u", map.index,
			     port_map_offset_x, port_map_offset_y, composite_count);
		}
		return;
	}

	// The map that was drawn a moment ago is gone: this is the frame that shows
	// in the game's own resolution. It is logged and written out once per
	// transition, so a flash that is still seen can be looked at directly.
	if (port_map_drew_last) {
		if (port_map_loss_logs < 40) {
			++port_map_loss_logs;
			diag("PORTDROP frame=%u off=%d,%d score=%d rel=%d scaled=%d fit=%.3f/%.3f/%.3f dark=%d best=%d known=%d",
			     composite_count, keep_x * PortMapStep, keep_y * PortMapStep,
			     keep_score, relation_keep, scaled_ok ? 1 : 0, scale[0],
			     scale[1], scale[2], dark_pixels(source, source_width),
			     best_any, known_any);
		}
		if (port_map_loss_dumps < 12) {
			++port_map_loss_dumps;
			std::filesystem::create_directories("sea2_portloss");
			char name[64] = {};
			std::snprintf(name, sizeof(name), "sea2_portloss/drop%05u.png",
			              composite_count);
			dump_port_window(source, source_width, name);
		}
	}

	// The map may still be there, only shifted or recoloured; such a frame is
	// logged and kept, because it is the one that shows for a moment before the
	// artwork comes back. A screen that is still black is skipped: that is the
	// game painting, not the map.
	if (port_map_miss_logs < 40) {
		++port_map_miss_logs;
		diag("PORTMISSLOST frame=%u best=%d known=%d dark=%d", composite_count,
		     best_any, known_any, dark_pixels(source, source_width));
	}
	if (port_map_miss_logs <= 4 && best_any >= 20) {
		std::filesystem::create_directories("sea2_portloss");
		char name[64] = {};
		std::snprintf(name, sizeof(name), "sea2_portloss/miss%05u.png",
		              composite_count);
		dump_port_window(source, source_width, name);
	}

	// Something else is on screen. The first few misses are written out, because
	// they say whether the window is simply another screen or whether the map is
	// drawn with colours that no longer match.
	if (port_map_diags < PortMapDiagCount && composite_count % 30 == 0) {
		++port_map_diags;
		std::unordered_map<uint32_t, int> colours = {};
		for (int y = 0; y < PortMapWindowSize; y += 8) {
			const auto* row = source +
			                  static_cast<size_t>(PortMapWindowY + y) *
			                          source_width +
			                  PortMapWindowX;
			for (int x = 0; x < PortMapWindowSize; x += 8) {
				++colours[row[x] & 0x00ffffff];
			}
		}
		diag("PORTMISS frame=%u keep=%d best=%d known=%d", composite_count,
		     keep_score, best_any, known_any);
		for (auto i = 0; i < 4 && !colours.empty(); ++i) {
			auto top = colours.begin();
			for (auto it = colours.begin(); it != colours.end(); ++it) {
				if (it->second > top->second) {
					top = it;
				}
			}
			diag("PORTMISS colour %06x n=%d", static_cast<unsigned>(top->first),
			     top->second);
			colours.erase(top);
		}
	}
	port_map_retry = PortMapRetryFrames;
}

// The artwork keeps the shape of every tile but not its brightness: the map is
// dithered, the artwork is not, and where the game draws black outlines the
// artwork averages a pale blue. Drawing the artwork as it is would lighten the
// whole port, in every time of day. The average colour of the artwork inside
// each flat colour of the map is therefore measured once and subtracted again,
// so the port keeps the brightness the game's palette gave it while the artwork
// only adds its detail.
void ensure_port_art_means(const PortMap& map,
                           const std::vector<uint32_t>& artwork)
{
	if (map.art_means_ready) {
		return;
	}
	const auto art_stride = PortMapSize * HdScale;
	std::unordered_map<uint32_t, std::array<double, 4>> sums = {};
	for (int y = 0; y < PortMapSize; y += 2) {
		const auto* base_row = map.pixels.data() +
		                       static_cast<size_t>(y) * PortMapSize;
		for (int x = 0; x < PortMapSize; x += 2) {
			const auto base = base_row[x] & 0x00ffffffu;
			auto& sum       = sums[base];
			sum[3] += 4.0;
			for (int dy = 0; dy < HdScale; dy += 2) {
				const auto* art_row = artwork.data() +
				        static_cast<size_t>(y) * HdScale * art_stride +
				        static_cast<size_t>(x) * HdScale +
				        static_cast<size_t>(dy) * art_stride;
				for (int dx = 0; dx < HdScale; dx += 2) {
					const auto art = art_row[dx];
					sum[0] += static_cast<double>((art >> 16) & 0xff);
					sum[1] += static_cast<double>((art >> 8) & 0xff);
					sum[2] += static_cast<double>(art & 0xff);
				}
			}
		}
	}
	auto& table = map.art_means;
	table.clear();
	for (const auto& [colour, sum] : sums) {
		if (sum[3] <= 0.0) {
			continue;
		}
		table[colour] = {static_cast<float>(sum[0] / sum[3]),
		                 static_cast<float>(sum[1] / sum[3]),
		                 static_cast<float>(sum[2] / sum[3])};
	}
	map.art_means_ready = true;
}

// Draws the window through the palette that was read off the same frame. A pixel
// is the map exactly when the palette turned its map colour into the colour the
// frame shows there, so the character, an item, a dialog or the pointer can never
// be painted over. The artwork keeps that colour and only adds its own detail on
// top of it, so the port looks the way the game's palette made it look.
void draw_port_map_measured(const PortMap& map,
                            const std::vector<uint32_t>& artwork,
                            const uint32_t* source, const int source_width,
                            uint32_t* destination, const int destination_stride)
{
	const auto& pairs = port_map_palette_pairs;
	// A map colour normally becomes exactly one screen colour. While the game
	// fades its palette in entry by entry, two of its entries hold the very same
	// map colour and then one of them is reached first: the same map pixel is
	// shown in either of the two colours, so both are looked up and the one the
	// frame really holds is used.
	const auto screen_colours_of = [&pairs](const uint32_t colour) {
		const auto first = std::lower_bound(
		        pairs.begin(), pairs.end(),
		        std::pair<uint32_t, uint32_t>{colour, 0u});
		auto last = first;
		while (last != pairs.end() && last->first == colour) {
			++last;
		}
		return std::make_pair(first, last);
	};
	ensure_port_art_means(map, artwork);
	const auto art_mean_of = [&map](const uint32_t colour) -> const float* {
		const auto it = map.art_means.find(colour);
		return it != map.art_means.end() ? it->second.data() : nullptr;
	};

	const auto art_stride = PortMapSize * HdScale;
	const auto channel    = [](const int value) {
		return static_cast<uint32_t>(std::clamp(value, 0, 255));
	};

	// Mark the pixels of a panel the game painted over the port. Only a pixel
	// that sits in a long flat run across *and* down is paper: every one of the
	// map's tiles is dithered, so the map never has such an area.
	auto* panel = port_map_panel.data();
	if (!port_map_panel_ready) {
		port_panel_mask(source, source_width, panel);
	}
	for (int ty = 0; ty < PortMapCells; ++ty) {
		for (int tx = 0; tx < PortMapCells; ++tx) {
			const auto source_x = PortMapWindowX + tx * PortMapStep;
			const auto source_y = PortMapWindowY + ty * PortMapStep;
			const auto map_x    = port_map_offset_x + tx * PortMapStep;
			const auto map_y    = port_map_offset_y + ty * PortMapStep;
			for (int y = 0; y < PortMapStep; ++y) {
				const auto* row = source +
				                  static_cast<size_t>(source_y + y) * source_width +
				                  source_x;
				const auto* base_row = map.pixels.data() +
				                       static_cast<size_t>(map_y + y) * PortMapSize +
				                       map_x;
				const auto* art_row = artwork.data() +
				                      static_cast<size_t>(map_y + y) * HdScale *
				                              art_stride +
				                      static_cast<size_t>(map_x) * HdScale;
				auto* out_row = destination +
				                static_cast<size_t>(source_y + y) * HdScale *
				                        destination_stride +
				                static_cast<size_t>(source_x) * HdScale;
				for (int x = 0; x < PortMapStep; ++x) {
					const auto base          = base_row[x] & 0x00ffffffu;
					const auto [first, last] = screen_colours_of(base);
					auto hit                 = last;
					for (auto it = first; it != last; ++it) {
						if (it->second == (row[x] & 0x00ffffffu)) {
							hit = it;
							break;
						}
					}
					if (hit == last) {
						continue;
					}
					const auto wants = hit->second;
					// A pixel of a panel over the port keeps the game's own
					// pixels, even where the paper happens to be one of the
					// map's colours.
					if ((panel[static_cast<size_t>(ty * PortMapStep + y) *
					                   PortMapWindowSize +
					           tx * PortMapStep + x] &
					     3) == 3) {
						continue;
					}
					const auto base_r    = static_cast<int>((base >> 16) & 0xff);
					const auto base_g    = static_cast<int>((base >> 8) & 0xff);
					const auto base_b    = static_cast<int>(base & 0xff);
					const auto screen_r  = static_cast<int>((wants >> 16) & 0xff);
					const auto screen_g  = static_cast<int>((wants >> 8) & 0xff);
					const auto screen_b  = static_cast<int>(wants & 0xff);
					const auto* mean     = art_mean_of(base);
					const auto mean_r    = mean ? static_cast<int>(mean[0] + 0.5f)
					                            : base_r;
					const auto mean_g    = mean ? static_cast<int>(mean[1] + 0.5f)
					                            : base_g;
					const auto mean_b    = mean ? static_cast<int>(mean[2] + 0.5f)
					                            : base_b;
					auto* block          = out_row + x * HdScale;
					const auto* art_block = art_row + x * HdScale;
					for (int dy = 0; dy < HdScale; ++dy) {
						auto* line = block +
						             static_cast<size_t>(dy) *
						                     destination_stride;
						const auto* art_line = art_block +
						                       static_cast<size_t>(dy) * art_stride;
						for (int dx = 0; dx < HdScale; ++dx) {
							const auto art = art_line[dx];
							const auto r   = channel(screen_r - mean_r +
							                         static_cast<int>((art >> 16) & 0xff));
							const auto g   = channel(screen_g - mean_g +
							                         static_cast<int>((art >> 8) & 0xff));
							const auto b   = channel(screen_b - mean_b +
							                         static_cast<int>(art & 0xff));
							line[dx] = (r << 16) | (g << 8) | b;
						}
					}
				}
			}
		}
	}
}

// Every cell the game did not take from the map is left exactly as the game
// drew it, so the character keeps walking over the artwork instead of through
// it. A cell that is mostly something else - a dialog, a label, the pointer -
// is skipped as a whole, so no map pixel leaks into a panel that happens to use
// one of its colours.
void draw_port_map(const uint32_t* source, const int source_width,
                   uint32_t* destination, const int destination_stride)
{
	port_map_drew_last = false;
	// 0 = the game's own pixels, 1 = artwork through the stored palette, 2 = the
	// artwork through the palette read off the frame. The port must never switch
	// between these from one frame to the next while the character walks, so
	// every switch is written out.
	auto state = 0;
	if (port_map_visible && port_map_selected >= 0) {
		const auto& art = port_map_art.find(
		        port_maps[static_cast<size_t>(port_map_selected)].index);
		if (art != port_map_art.end() && !art->second.empty()) {
			state = port_map_measured ? 2 : 1;
		}
	}
	if (state != port_map_last_state && port_map_state_logs < 80) {
		++port_map_state_logs;
		diag("PORTSTATE %d->%d frame=%u x=%d y=%d sel=%d",
		     port_map_last_state, state, composite_count, port_map_offset_x,
		     port_map_offset_y, port_map_selected);
	}
	port_map_last_state = state;

	if (!port_map_visible || port_map_selected < 0) {
		return;
	}
	const auto& map = port_maps[static_cast<size_t>(port_map_selected)];
	const auto found = port_map_art.find(map.index);
	if (found == port_map_art.end() || found->second.empty()) {
		return;
	}
	const auto& artwork = found->second;
	port_map_drew_last = true;
	if (port_map_measured && !port_map_palette_pairs.empty()) {
		draw_port_map_measured(map, artwork, source, source_width, destination,
		                       destination_stride);
		return;
	}
	const auto faded    = port_map_fade[0] < 0.999f ||
	                   port_map_fade[1] < 0.999f || port_map_fade[2] < 0.999f;
	// The time of day and, while the game fades its screen, the fade as well.
	const std::array<float, 3> shade = {map.gain[0] * port_map_fade[0],
	                                    map.gain[1] * port_map_fade[1],
	                                    map.gain[2] * port_map_fade[2]};
	const auto tinted   = faded || shade[0] < 0.999f || shade[1] < 0.999f ||
	                    shade[2] < 0.999f || shade[0] > 1.001f ||
	                    shade[1] > 1.001f || shade[2] > 1.001f;
	// The artwork was drawn for one time of day; the other three scale it the way
	// the game scales its own palette, so the port keeps its morning, evening and
	// night look instead of staying as bright as noon.
	const auto tint = [&shade](const uint32_t colour) {
		const auto r = std::min(255, static_cast<int>(((colour >> 16) & 0xff) *
		                                              shade[0]));
		const auto g = std::min(255, static_cast<int>(((colour >> 8) & 0xff) *
		                                              shade[1]));
		const auto b = std::min(255, static_cast<int>((colour & 0xff) * shade[2]));
		return static_cast<uint32_t>((r << 16) | (g << 8) | b);
	};
	const auto same_colour = [&shade, faded](const uint32_t actual,
	                                         const uint32_t expected) {
		if (!faded) {
			return port_colour(actual) == port_colour(expected);
		}
		const auto dr = static_cast<int>((actual >> 16) & 0xff) -
		                std::min(255,
		                         static_cast<int>(((expected >> 16) & 0xff) *
		                                          shade[0]));
		const auto dg = static_cast<int>((actual >> 8) & 0xff) -
		                std::min(255,
		                         static_cast<int>(((expected >> 8) & 0xff) *
		                                          shade[1]));
		const auto db = static_cast<int>(actual & 0xff) -
		                std::min(255,
		                         static_cast<int>((expected & 0xff) * shade[2]));
		return dr >= -14 && dr <= 14 && dg >= -14 && dg <= 14 && db >= -14 &&
		       db <= 14;
	};

	const auto art_stride = PortMapSize * HdScale;
	// Reconnaissance: which cells were replaced this frame and which were left
	// alone, so a region that stays low resolution can be looked at directly.
	const auto trace_cells = port_map_diags < PortMapDiagCount &&
	                         composite_count % 30 == 0;
	char states[PortMapCellCount + 1] = {};
	for (int ty = 0; ty < PortMapCells; ++ty) {
		for (int tx = 0; tx < PortMapCells; ++tx) {
			const auto source_x = PortMapWindowX + tx * PortMapStep;
			const auto source_y = PortMapWindowY + ty * PortMapStep;
			const auto map_x    = port_map_offset_x + tx * PortMapStep;
			const auto map_y    = port_map_offset_y + ty * PortMapStep;

			int same = 0;
			for (int y = 0; y < PortMapStep; ++y) {
				const auto* row = source +
				                  static_cast<size_t>(source_y + y) * source_width +
				                  source_x;
				const auto* expected = map.pixels.data() +
				                       static_cast<size_t>(map_y + y) * PortMapSize +
				                       map_x;
				for (int x = 0; x < PortMapStep; ++x) {
					same += same_colour(row[x], expected[x]) ? 1 : 0;
				}
			}
			const auto whole = same == PortMapStep * PortMapStep;
			if (!whole && same < PortMapPartialCell) {
				if (trace_cells) {
					states[ty * PortMapCells + tx] = 'x';
				}
				continue;
			}
			if (trace_cells) {
				states[ty * PortMapCells + tx] = whole ? 'W' : 'p';
			}

			for (int y = 0; y < PortMapStep; ++y) {
				const auto* row = source +
				                  static_cast<size_t>(source_y + y) * source_width +
				                  source_x;
				const auto* expected = map.pixels.data() +
				                       static_cast<size_t>(map_y + y) * PortMapSize +
				                       map_x;
				const auto* art = artwork.data() +
				                  static_cast<size_t>(map_y + y) * HdScale *
				                          art_stride +
				                  map_x * HdScale;
				auto* out = destination +
				            static_cast<size_t>(source_y + y) * HdScale *
				                    destination_stride +
				            static_cast<size_t>(source_x) * HdScale;
				for (int x = 0; x < PortMapStep; ++x) {
					if (!whole && !same_colour(row[x], expected[x])) {
						continue;
					}
					auto* block = out + x * HdScale;
					const auto* art_block = art + x * HdScale;
					for (int dy = 0; dy < HdScale; ++dy) {
						auto* line = block +
						             static_cast<size_t>(dy) *
						                     destination_stride;
						const auto* art_line = art_block +
						                       static_cast<size_t>(dy) * art_stride;
						if (!tinted) {
							std::memcpy(line, art_line,
							            HdScale * sizeof(uint32_t));
							continue;
						}
						for (int dx = 0; dx < HdScale; ++dx) {
							line[dx] = tint(art_line[dx]);
						}
					}
				}
			}
		}
	}

	if (trace_cells) {
		auto skipped = 0;
		for (const auto state : states) {
			skipped += state == 'x' ? 1 : 0;
		}
		if (skipped >= 4) {
			diag_port_cells(source, source_width, destination,
			                destination_stride, states);
		}
	}
}

std::array<float, 3> port_mean_colour(const std::vector<uint32_t>& pixels)
{
	double sum[3] = {0.0, 0.0, 0.0};
	for (const auto colour : pixels) {
		sum[0] += static_cast<double>((colour >> 16) & 0xff);
		sum[1] += static_cast<double>((colour >> 8) & 0xff);
		sum[2] += static_cast<double>(colour & 0xff);
	}
	const auto scale = pixels.empty()
	                         ? 0.0
	                         : 1.0 / static_cast<double>(pixels.size());
	return {static_cast<float>(sum[0] * scale),
	        static_cast<float>(sum[1] * scale),
	        static_cast<float>(sum[2] * scale)};
}

int port_map_index_of(const std::string& name)
{
	if (name.size() < 4) {
		return -1;
	}
	int index = 0;
	for (auto i = 0; i < 3; ++i) {
		const auto digit = name[static_cast<size_t>(i)];
		if (digit < '0' || digit > '9') {
			return -1;
		}
		index = index * 10 + (digit - '0');
	}
	return index;
}

// Loads one palette of a port map. The map exists once per time of day, because
// the game recolours the whole port as the day passes; the palette the artwork
// was drawn for is the reference that the others are scaled against.
bool load_port_map(const uint32_t index, const std::string& filename,
                   const bool reference)
{
	const auto path = (assets_root / "PORTMAP" / filename).string();

	auto* loaded = IMG_Load(path.c_str());
	if (!loaded) {
		LOG_WARNING("SEA2HD: Cannot load port map %s: %s", filename.c_str(),
		            IMG_GetError());
		return false;
	}
	auto* converted = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
	SDL_FreeSurface(loaded);
	if (!converted) {
		return false;
	}
	if (converted->w != PortMapSize || converted->h != PortMapSize) {
		LOG_WARNING("SEA2HD: Port map %s has invalid dimensions %dx%d",
		            filename.c_str(), converted->w, converted->h);
		SDL_FreeSurface(converted);
		return false;
	}

	PortMap map = {};
	map.index   = index;
	map.pixels.resize(static_cast<size_t>(PortMapSize) * PortMapSize);

	if (SDL_LockSurface(converted) != 0) {
		SDL_FreeSurface(converted);
		return false;
	}
	for (int y = 0; y < PortMapSize; ++y) {
		const auto* row = static_cast<const uint8_t*>(converted->pixels) +
		                  static_cast<size_t>(y) * converted->pitch;
		auto* out = map.pixels.data() + static_cast<size_t>(y) * PortMapSize;
		for (int x = 0; x < PortMapSize; ++x) {
			out[x] = (static_cast<uint32_t>(row[x * 4 + 0]) << 16) |
			         (static_cast<uint32_t>(row[x * 4 + 1]) << 8) |
			         static_cast<uint32_t>(row[x * 4 + 2]);
		}
	}
	SDL_UnlockSurface(converted);
	SDL_FreeSurface(converted);

	// The grid is what every frame is matched against, so the hashes are built
	// once here instead of on every frame.
	for (int cell_y = 0; cell_y < PortMapGridSize; ++cell_y) {
		for (int cell_x = 0; cell_x < PortMapGridSize; ++cell_x) {
			const auto* cell_pixels =
			        map.pixels.data() +
			        static_cast<size_t>(cell_y * PortMapStep) * PortMapSize +
			        cell_x * PortMapStep;
			const auto hash =
			        hash_port_cell(cell_pixels, PortMapSize);
			const auto cell = static_cast<uint16_t>(
			        cell_y * PortMapGridSize + cell_x);
			map.cells[cell] = hash;
			map.cell_index[hash].push_back(cell);

			std::array<uint64_t, PortMapRelationWords> relations = {};
			port_cell_relations(cell_pixels, PortMapSize, relations);
			const auto relation_hash = hash_port_relations(relations);
			map.relation_hashes[cell] = relation_hash;
			map.relation_index[relation_hash].push_back(cell);
		}
	}

	const auto mean = port_mean_colour(map.pixels);
	if (reference || port_map_palette.find(index) == port_map_palette.end()) {
		port_map_palette[index] = mean;
	} else {
		const auto& base = port_map_palette[index];
		for (auto i = 0; i < 3; ++i) {
			const auto channel = static_cast<size_t>(i);
			const auto ratio   = base[channel] > 1.0f
			                             ? mean[channel] / base[channel]
			                             : 1.0f;
			map.gain[channel]  = std::clamp(ratio, 0.30f, 1.60f);
		}
	}

	port_maps.push_back(std::move(map));
	return true;
}

// The artwork is one image for the whole port, whatever the time of day.
bool load_port_map_art(const uint32_t index)
{
	if (port_map_art.find(index) != port_map_art.end()) {
		return true;
	}

	char name[16] = {};
	std::snprintf(name, sizeof(name), "%03u_hd.png", index);
	const auto path = (assets_root / "PORTMAP" / name).string();

	auto* loaded = IMG_Load(path.c_str());
	if (!loaded) {
		return false;
	}
	auto* art = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
	SDL_FreeSurface(loaded);
	const auto art_size = PortMapSize * HdScale;
	if (!art || art->w != art_size || art->h != art_size ||
	    SDL_LockSurface(art) != 0) {
		if (art) {
			SDL_FreeSurface(art);
		}
		LOG_WARNING("SEA2HD: Port map %u artwork is missing or unusable", index);
		return false;
	}

	auto& pixels = port_map_art[index];
	pixels.resize(static_cast<size_t>(art_size) * art_size);
	for (int y = 0; y < art_size; ++y) {
		const auto* row = static_cast<const uint8_t*>(art->pixels) +
		                  static_cast<size_t>(y) * art->pitch;
		auto* out = pixels.data() + static_cast<size_t>(y) * art_size;
		for (int x = 0; x < art_size; ++x) {
			out[x] = (static_cast<uint32_t>(row[x * 4 + 0]) << 16) |
			         (static_cast<uint32_t>(row[x * 4 + 1]) << 8) |
			         static_cast<uint32_t>(row[x * 4 + 2]);
		}
	}
	SDL_UnlockSurface(art);
	SDL_FreeSurface(art);
	LOG_MSG("SEA2HD: Loaded port map %u artwork", index);
	return true;
}

// Every file in the folder is one palette of one map: `002.png` is the palette
// the artwork was drawn for, `002_night.png` another time of day of the same map
// and `002_hd.png` the artwork itself.
void preload_port_maps()
{
	if (port_maps_loaded) {
		return;
	}
	port_maps_loaded = true;

	const auto dir = assets_root / "PORTMAP";
	std::error_code error;
	const auto started = SDL_GetTicks64();

	std::vector<std::string> names = {};
	for (const auto& file : std::filesystem::directory_iterator(dir, error)) {
		names.push_back(file.path().filename().string());
	}
	if (error) {
		return;
	}

	const auto is_png = [](const std::string& name) {
		return name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0;
	};

	// The reference palettes first, so the others can be scaled against them.
	for (const auto& name : names) {
		if (name.size() != 7 || !is_png(name)) {
			continue;
		}
		const auto index = port_map_index_of(name);
		if (index >= 0) {
			load_port_map(static_cast<uint32_t>(index), name, true);
		}
	}
	for (const auto& name : names) {
		if (name.size() <= 7 || name[3] != '_' || !is_png(name) ||
		    name.compare(4, 2, "hd") == 0) {
			continue;
		}
		const auto index = port_map_index_of(name);
		if (index >= 0) {
			load_port_map(static_cast<uint32_t>(index), name, false);
		}
	}
	for (const auto& name : names) {
		if (name.size() <= 7 || name[3] != '_' || !is_png(name) ||
		    name.compare(4, 2, "hd") != 0) {
			continue;
		}
		const auto index = port_map_index_of(name);
		if (index >= 0) {
			load_port_map_art(static_cast<uint32_t>(index));
		}
	}

	LOG_MSG("SEA2HD: Preloaded %zu port map palettes in %llu ms",
	        port_maps.size(),
	        static_cast<unsigned long long>(SDL_GetTicks64() - started));
}

void SEA2_HD_ClearProgram()
{
	sea2_hd_trace_loadseg = 0;
	main_pspseg = 0;
	trace_event_count = 0;
	draw_commands.clear();
	text_cells.clear();
	pending_glyph_record = 0;
	if (hd_enabled) {
		LOG_MSG("SEA2HD: glyph bitmaps indexed %u; text cells %u, misses %u",
		        glyph_index_used,
		        glyph_match_total,
		        glyph_miss_total);
	}
	reset_glyph_index();
	frame_dirty = true;
}

void SEA2_HD_NotifyProgramExecuted(const uint16_t pspseg, const uint16_t loadseg,
                                   const char* canonical_name)
{
	if ((!trace_enabled && !hd_enabled) || !canonical_name ||
	    !iequals(canonical_name, "C:\\MAIN.EXE")) {
		return;
	}

	SEA2_HD_ClearProgram();
	if (!main_entrypoints_match(loadseg)) {
		LOG_MSG("SEA2HD: MAIN.EXE entry signatures do not match; HD disabled");
		return;
	}

	main_pspseg = pspseg;
	sea2_hd_trace_loadseg = loadseg;
	LOG_MSG("SEA2HD: MAIN.EXE matched, PSP=%04X loadseg=%04X", pspseg, loadseg);
	SEA2_HD_VerifyGlyphMap();
	if (hd_enabled) {
		preload_kao64_assets();
		preload_port_maps();
	}
}

void SEA2_HD_NotifyProgramTerminated(const uint16_t pspseg)
{
	if (pspseg == main_pspseg && sea2_hd_trace_loadseg != 0) {
		LOG_MSG("SEA2HD: MAIN.EXE terminated; HD disabled");
		SEA2_HD_ClearProgram();
	}
}

void SEA2_HD_TraceKaoEntry(const uint16_t ip)
{
	if (hd_enabled && ip == Kao64Entry) {
		record_kao64_command();
	}

	if (hd_enabled && ip == Kao48Entry) {
		static uint32_t kao48_lines = 0;
		if (kao48_lines < 240) {
			++kao48_lines;
			diag("KAO48 ax=%u bx=%u dx=%u", reg_ax, reg_bx, reg_dx);
		}
	}

	if (!trace_enabled || trace_event_count >= MaxTraceEvents) {
		return;
	}

	const auto event_number = trace_event_count++;
	const auto return_ip = real_readw(SegValue(ss), reg_sp);
	const auto return_cs = real_readw(SegValue(ss), reg_sp + 2);
	LOG_MSG("SEA2HD: event=%u kind=%s caller=%04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X entry48=%04X",
	        event_number,
	        ip == Kao64Entry ? "KAO64" : "KAO48",
	        return_cs,
	        return_ip,
	        reg_ax,
	        reg_bx,
	        reg_cx,
	        reg_dx,
	        reg_si,
	        reg_di,
	        reg_bp,
	        reg_sp,
	        SegValue(ds),
	        SegValue(es),
	        static_cast<uint16_t>(reg_bx + 0x80));
}

void SEA2_HD_Composite(const uint32_t* source, const int source_width,
                       const int source_height, uint32_t* destination,
                       const int destination_pitch)
{
	if (!source || !destination || source_width <= 0 || source_height <= 0 ||
	    destination_pitch < source_width * HdScale * 4) {
		return;
	}

	const auto destination_stride = destination_pitch / 4;

	// Nearest-neighbour 4x is only the fallback for unsupported pixels. KAO
	// pixels below come directly from their RGBA HQX assets, never from this
	// BGRX frame and never through palette quantisation.
	for (int y = 0; y < source_height; ++y) {
		auto* out = destination + static_cast<size_t>(y) * HdScale * destination_stride;
		const auto* in = source + y * source_width;
		for (int x = 0; x < source_width; ++x) {
			const auto color = in[x];
			out[x * 4 + 0] = color;
			out[x * 4 + 1] = color;
			out[x * 4 + 2] = color;
			out[x * 4 + 3] = color;
		}
		const auto row_bytes = static_cast<size_t>(source_width) * HdScale * 4;
		for (int dy = 1; dy < HdScale; ++dy) {
			std::memcpy(out + dy * destination_stride, out, row_bytes);
		}
	}

	if (source_width == VgaWidth && source_height == VgaHeight) {
		update_port_map(source, source_width);
		draw_port_map(source, source_width, destination,
		              destination_stride);
	}

	// One brightness sample per cell of a coarse grid, compared with the frame a
	// whole window of frames older. A fade repaints the entire screen, while a
	// dialog, a label or the pointer only repaints part of it.
	LumaGrid luma_grid = {};
	for (int cell_y = 0; cell_y < LumaRows; ++cell_y) {
		const auto y = (cell_y * source_height) / LumaRows;
		const auto* line = source + static_cast<size_t>(y) * source_width;
		for (int cell_x = 0; cell_x < LumaCols; ++cell_x) {
			const auto x = (cell_x * source_width) / LumaCols;
			luma_grid[cell_y * LumaCols + cell_x] =
			        static_cast<uint8_t>(channel_sum(line[x]) / 3);
		}
	}
	const bool have_history = luma_history_count == LumaHistorySize;
	int changed_cells       = 0;
	int drift_sum           = 0;
	if (have_history) {
		const auto& oldest_grid = luma_history[luma_history_next];
		for (size_t i = 0; i < luma_grid.size(); ++i) {
			const auto difference = static_cast<int>(luma_grid[i]) -
			                        static_cast<int>(oldest_grid[i]);
			if (difference > 4 || difference < -4) {
				++changed_cells;
			}
			drift_sum += difference;
		}
	}
	luma_history[luma_history_next] = luma_grid;
	luma_history_next = (luma_history_next + 1) % LumaHistorySize;
	if (luma_history_count < LumaHistorySize) {
		++luma_history_count;
	}

	const bool whole_screen_moved = changed_cells * 100 >= LumaCells * 35;
	const bool brightening        = drift_sum > 6 * LumaCells;
	const bool darkening          = drift_sum < -6 * LumaCells;
	screen_fading      = have_history && whole_screen_moved &&
	                (brightening || darkening);

	// The six fixed portrait boxes showing their picture together is what
	// identifies the character selection screen. Only boxes that still hold that
	// picture count: the commands themselves stay in the list for the rest of the
	// session, so counting them would keep the screen "active" long after it is
	// gone and paint portraits over the next one.
	int scene_intact     = 0;
	int scene_locked     = 0;
	int scene_detailed   = 0;
	for (const auto& command : draw_commands) {
		if (!is_scene_portrait_box(command)) {
			continue;
		}
		if (!command.locked) {
			continue;
		}
		++scene_locked;
		const auto* region = kao_box_origin(command, source, source_width);
		std::array<uint8_t, KaoRelationBytes> bits = {};
		build_relation_bits(region, source_width, bits);
		const auto structure = count_relation_structure(bits);
		if (structure < KaoMinRelationStructure) {
			continue;
		}
		const auto change = compare_relation_bits(command.reference, bits);
		if (change.total() <= KaoSceneIntactTolerance) {
			++scene_intact;
		}
		// A portrait is full of edges, so its pattern is dense. Flat water, a
		// harbour, a menu or any other screen leaves the box far flatter, which
		// is what tells the next screen from this one even when its flat areas
		// happen to fall on the same side of a neighbour comparison.
		if (structure * 2 >= count_relation_structure(command.reference)) {
			++scene_detailed;
		}
	}

	// The commands of the selection screen are removed once that screen is gone,
	// instead of being followed for the rest of the session. Two independent
	// signs say it is gone, and both only ever remove commands, so the frame by
	// frame rules above stay exactly as they are.
	//
	// The game paints every screen change from or to black; the selection screen
	// itself fades in from black before it is drawn, so a black frame after it
	// was lit can only be the way out.
	int screen_luma = 0;
	for (const auto cell : luma_grid) {
		screen_luma += cell;
	}
	screen_luma /= LumaCells;
	// The fade out passes through every brightness, so the lit state has to be
	// remembered until the black frame is reached, not recomputed every frame.
	if (screen_luma >= KaoLitScreenLuma) {
		screen_has_been_lit = true;
	}
	if (screen_luma <= KaoBlackScreenLuma && screen_has_been_lit) {
		drop_scene_commands();
		screen_has_been_lit = false;
	}

	// A screen that is entered without a fade is caught by the second sign: the
	// six portraits together are dense with edges, so a stretch of frames in
	// which the frozen boxes are no longer dense is the end of the screen just
	// as well. The delay keeps a dialog that keeps several boxes busy for a while
	// from counting as a different screen.
	if (scene_locked >= KaoSceneMinIntactBoxes) {
		if (scene_detailed < KaoSceneMinDetailedBoxes) {
			if (scene_lost_frames < KaoSceneRetireFrames) {
				++scene_lost_frames;
			}
			if (scene_lost_frames >= KaoSceneRetireFrames) {
				drop_scene_commands();
			}
		} else {
			scene_lost_frames = 0;
		}
	}

	portrait_scene_active = scene_intact >= KaoSceneMinIntactBoxes;

	// The blit order above is what normally hides the pointer. Should the game
	// ever draw the six boxes in a way this misses, the recognised screen is
	// the second, later point at which the same is done.
	if (portrait_scene_active && !scene_cursor_suppressed) {
		scene_cursor_suppressed = true;
		MOUSEDOS_SuppressCursorUntilMove();
		diag("HIDE pointeractive frame=%u", composite_count);
	}

	for (auto it = draw_commands.begin(); it != draw_commands.end(); ++it) {
		// Each portrait box is followed by exactly one of the two algorithms for
		// its whole life: the six fixed boxes of the character selection screen
		// by the scene rule, every other box by the generic rule. They are
		// dispatched here and never call each other except for the scene rule
		// handing a box back when that screen is gone.
		const bool shown = is_scene_portrait_box(*it)
		                         ? scene_box_shows_portrait(*it, source,
		                                                    source_width, source_height)
		                         : dialog_box_shows_portrait(*it, source,
		                                                     source_width,
		                                                     source_height);
		if (!shown) {
			continue;
		}

		const auto& command = *it;
		const auto* asset = get_kao64_asset(command.entry);
		if (!asset) {
			continue;
		}

		// A screen fade dims the game's own pixels, and the artwork has to follow
		// it, otherwise the portrait stays lit while everything else darkens and
		// then pops out. The gain and offset that fit the stored pixels onto this
		// frame are applied to the artwork as well.
		const bool modulate = screen_fading &&
		                      (command.fade_gain[0] < 0.999f ||
		                       command.fade_gain[1] < 0.999f ||
		                       command.fade_gain[2] < 0.999f);
		const auto fade_channel = [&command](const uint8_t value, const int channel) {
			const auto scaled = static_cast<double>(command.fade_gain[channel]) *
			                            static_cast<double>(value) +
			                    static_cast<double>(command.fade_offset[channel]);
			return static_cast<uint8_t>(std::clamp(scaled, 0.0, 255.0));
		};

		for (int y = 0; y < Kao64Height * HdScale; ++y) {
			const auto* src = asset->rgba.data() +
			                  static_cast<size_t>(y) * Kao64Width * HdScale * 4;
			// A window, a label or the pointer sits there: leave those blocks
			// alone so the game's own pixels stay on top of the portrait.
			const auto* covered_row =
			        command.covered.data() +
			        static_cast<size_t>(y / HdScale) * Kao64Width;
			auto* dst = destination +
			            (static_cast<size_t>(command.y + ContentYOffset) * HdScale + y) *
			                    destination_stride +
			            static_cast<size_t>(command.x) * HdScale;

			for (int x = 0; x < Kao64Width * HdScale; ++x) {
				if (covered_row[x / HdScale] != 0) {
					continue;
				}
				const auto alpha = src[x * 4 + 3];
				if (alpha == 0) {
					continue;
				}
				auto b = src[x * 4 + 0];
				auto g = src[x * 4 + 1];
				auto r = src[x * 4 + 2];
				if (modulate) {
					b = fade_channel(b, 0);
					g = fade_channel(g, 1);
					r = fade_channel(r, 2);
				}
				if (alpha == 255) {
					dst[x] = Bgrx8888{b, g, r};
					continue;
				}

				const auto previous = dst[x];
				const auto dr = static_cast<uint8_t>((previous >> 16) & 0xff);
				const auto dg = static_cast<uint8_t>((previous >> 8) & 0xff);
				const auto db = static_cast<uint8_t>(previous & 0xff);
				dst[x] = Bgrx8888{blend_channel(b, dr, alpha),
				                  blend_channel(g, dg, alpha),
				                  blend_channel(r, db, alpha)};
			}
		}

		if (composite_count % 60 == 0) {
			int covered_blocks = 0;
			for (const auto mark : command.covered) {
				covered_blocks += mark != 0 ? 1 : 0;
			}
			if (covered_blocks > 0) {
				diag("HDMASK entry=%u x=%u y=%u frame=%u covered=%d",
				     command.entry, command.x, command.y, composite_count,
				     covered_blocks);
			}
		}
	}

	// Simplified text glyphs are drawn after the portraits so they always win.
	// A cell that no longer looks like a glyph is simply skipped this frame;
	// it is never deleted, so the next redraw brings the text back.
	if (source_width == VgaWidth && source_height == VgaHeight) {
		uint16_t pointer_x      = 0;
		uint16_t pointer_y      = 0;
		const bool have_pointer = MOUSEDOS_GetCursorPosition(pointer_x, pointer_y);
		const bool log_hover    = have_pointer && composite_count % 30 == 0;
		int hover_logged        = 0;
		if (log_hover) {
			diag("TXTHOVER frame=%u pointer=%u,%u cells=%zu", composite_count,
			     pointer_x, pointer_y, text_cells.size());
		}
		// Text is written in whole lines, so the frame is handled in two passes:
		// first every cell is read on its own, which also records the colours
		// its line is painted with, then the cells the pointer hides borrow
		// both from a cell beside them.
		const auto pointer_reaches = [&](const TextCell& cell) {
			if (!have_pointer) {
				return false;
			}
			const int px = static_cast<int>(pointer_x);
			const int py = static_cast<int>(pointer_y);
			return px >= static_cast<int>(cell.x) - TextPointerMargin &&
			       px < static_cast<int>(cell.x) + TextGlyphWidth +
			                    TextPointerMargin &&
			       py >= static_cast<int>(cell.y) + ContentYOffset -
			                    TextPointerMargin &&
			       py < static_cast<int>(cell.y) + ContentYOffset +
			                    TextGlyphHeight + TextPointerMargin;
		};
		int seen_now    = 0;
		int seen_before = 0;
		int vanished    = 0;
		for (auto& cell : text_cells) {
			const bool was_seen  = cell.seen_last_frame;
			cell.seen_this_frame = false;
			cell.seen_last_frame = false;
			if (was_seen) {
				++seen_before;
			}
			uint16_t record     = 0;
			uint32_t background = 0;
			uint32_t ink        = 0;
			GlyphMask observed  = {};
			if (!identify_text_cell(source, source_width, cell.x, cell.y, record,
			                        background, ink, &observed)) {
				// Under the pointer a failure says nothing: the cell itself is
				// hidden. Anywhere else the glyph really is gone, and a few such
				// frames in a row drop the record, so a cell the screen change
				// left behind can never be painted back below the pointer.
				if (pointer_reaches(cell)) {
					continue;
				}
				if (cell.misses < TextCellMissLimit) {
					++cell.misses;
				}
				if (cell.misses >= TextCellMissLimit) {
					cell.verified = false;
				}
				// A cell that was readable a moment ago and is gone now also
				// marks a screen change.
				if (was_seen) {
					++vanished;
				}
				continue;
			}
			cell.misses = 0;
			// The glyph that is really on screen is the best reference for
			// the next time the pointer covers it.
			cell.known           = true;
			cell.record          = record;
			cell.mask            = observed;
			cell.background      = background;
			cell.ink             = ink;
			cell.seen_this_frame = true;
			cell.seen_last_frame = true;
			cell.verified        = true;
			++seen_now;
		}
		// The screen was replaced when most of the text that was readable a frame
		// ago is gone. Everything that cannot be read right now then loses its
		// trust, including the cells the pointer happens to hide: a record from
		// the previous screen is never painted again, which is what stops the
		// settings screen from leaving characters on the main menu.
		if (vanished >= 4 && vanished * 2 >= seen_before) {
			for (auto& cell : text_cells) {
				if (!cell.seen_this_frame) {
					cell.verified = false;
				}
			}
			diag("TXTRESET frame=%u seen=%d vanished=%d before=%d cells=%zu",
			     composite_count, seen_now, vanished, seen_before,
			     text_cells.size());
		}
		for (auto& cell : text_cells) {
			const bool near = log_hover && hover_logged < 8 &&
			                  std::abs(static_cast<int>(pointer_x) -
			                           static_cast<int>(cell.x)) <= 48 &&
			                  std::abs(static_cast<int>(pointer_y) -
			                           static_cast<int>(cell.y) - ContentYOffset) <= 48;
			uint16_t record     = 0;
			uint32_t background = 0;
			uint32_t ink        = 0;
			int identified      = 0;
			if (cell.seen_this_frame) {
				record     = cell.record;
				background = cell.background;
				ink        = cell.ink;
				identified = 1;
			} else if (cell.known && cell.verified && have_pointer &&
			           pointer_over_text_cell(cell.x, cell.y,
			                                  TextPointerMargin)) {
				// A cell the pointer hides cannot name its own character any
				// more, but the rest of its line can: one line is painted
				// with one ink and one paper. The closest cell of the same
				// line that was read this frame supplies both.
				TextCell* mate    = nullptr;
				int mate_distance = 0;
				for (auto& other : text_cells) {
					if (&other == &cell || !other.seen_this_frame ||
					    std::abs(static_cast<int>(other.y) -
					             static_cast<int>(cell.y)) > 1) {
						continue;
					}
					const int distance = std::abs(
					        static_cast<int>(other.x) -
					        static_cast<int>(cell.x));
					if (mate && distance >= mate_distance) {
						continue;
					}
					if (!text_cell_matches_colors(source, source_width,
					                              cell.x, cell.y, cell.mask,
					                              other.background,
					                              other.ink)) {
						continue;
					}
					mate          = &other;
					mate_distance = distance;
				}
				if (mate) {
					record     = cell.record;
					background = mate->background;
					ink        = mate->ink;
					identified = 3;
				} else if (identify_text_cell_while_covered(source, source_width,
				                                            cell.x, cell.y,
				                                            cell.mask, background,
				                                            ink)) {
					// No readable neighbour: the pixels still have to name
					// the ink and the paper on their own.
					record     = cell.record;
					identified = 2;
				}
			}
			if (identified == 0) {
				if (near) {
					diag("  TXTCELL x=%u y=%u ident=0 known=%d", cell.x, cell.y,
					     cell.known ? 1 : 0);
					++hover_logged;
				}
				continue;
			}
			const auto display_record = patched_text_record(cell, record);
			const bool drawn = draw_text_cell(display_record, cell.x, cell.y,
			                                  background, ink, destination,
			                                  destination_stride);
			// The tile covers the whole cell, so the pointer has to be put
			// back on top of it. Only pixels that carry neither the ink nor
			// the background are touched, so a cell the pointer merely passes
			// by stays fully simplified.
			if (drawn && have_pointer &&
			    pointer_over_text_cell(cell.x, cell.y, TextPointerMargin)) {
				restore_pointer_pixels(source, source_width, destination,
				                       destination_stride, cell.x, cell.y,
				                       cell.mask, background, ink);
			}
			if (near) {
				diag("  TXTCELL x=%u y=%u ident=%d record=%u disp=%u has=%d drawn=%d "
				     "bg=%06x ink=%06x",
				     cell.x, cell.y, identified, record, display_record,
				     has_simplified_glyph(display_record) ? 1 : 0, drawn ? 1 : 0,
				     background & 0xffffffu, ink & 0xffffffu);
				++hover_logged;
			}
		}
	}
	++composite_count;
	if (composite_count == 1) {
		diag("FIRST size=%dx%d", source_width, source_height);
	}
	if (source_width == VgaWidth && source_height == VgaHeight &&
	    (composite_count == 20 || composite_count == 60 ||
	     composite_count == 150 || composite_count == 400)) {
		diag("STATE frame=%u commands=%zu", composite_count,
		     draw_commands.size());
		for (auto& command : draw_commands) {
			std::array<uint8_t, KaoRelationBytes> bits = {};
			build_relation_bits(kao_box_origin(command, source, source_width),
			                    source_width,
			                    bits);
			diag("  cmd entry=%u x=%u y=%u locked=%d stable=%u unsettled=%u structure=%d diff=%d",
			     command.entry,
			     command.x,
			     command.y,
			     command.locked ? 1 : 0,
			     command.stable,
			     command.unsettled,
			     count_relation_structure(bits),
			     command.locked ? count_relation_differences(bits, command.reference)
			                    : -1);
		}
	}

	frame_dirty = false;
}
