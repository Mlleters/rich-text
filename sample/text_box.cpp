#include "text_box.hpp"

#include "config_vars.hpp"
#include "font_cache.hpp"
#include "image.hpp"
#include "pipeline.hpp"
#include "text_atlas.hpp"
#include "msdf_text_atlas.hpp"
#include "formatting_iterator.hpp"
#include "ui_container.hpp"
#include "cursor_controller.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace {

enum class PostLayoutCursorMoveType {
	LINE_START,
	LINE_END,
	LINE_ABOVE,
	LINE_BELOW,
	MOUSE_POSITION
};

struct PostLayoutCursorMove {
	PostLayoutCursorMoveType type;
	bool selectionMode;
};

struct CursorToMouse : public PostLayoutCursorMove {
	double mouseX;
	double mouseY;
};

}

static constexpr const double DOUBLE_CLICK_TIME = 0.5;

static Text::CursorController g_cursorCtrl;
static TextBox* g_focusedTextBox = nullptr;
static Text::CursorPositionResult g_cursorPos{};
static bool g_isMouseDown = false;
static double g_lastClickTime = 0.0;
static uint32_t g_clickCount = 0;
static CursorPosition g_lastClickPos{CursorPosition::INVALID_VALUE};

static CursorPosition apply_cursor_move(const Text::LayoutInfo& layout, float textWidth,
		TextXAlignment textXAlignment, const PostLayoutCursorMove& op, CursorPosition cursor);

std::shared_ptr<TextBox> TextBox::create() {
	return std::make_shared<TextBox>();
}

TextBox* TextBox::get_focused_text_box() {
	return g_focusedTextBox;
}

TextBox::~TextBox() {
	release_focus();
}

bool TextBox::handle_mouse_button(int button, int action, int mods, double mouseX, double mouseY) {
	if (button != GLFW_MOUSE_BUTTON_1) {
		return false;
	}

	bool mouseInside = is_mouse_inside(mouseX, mouseY);

	if (action == GLFW_PRESS) {
		if (g_focusedTextBox == this) {
			if (mouseInside) {
				cursor_move_to_mouse(mouseX - get_position()[0], mouseY - get_position()[1],
						mods & GLFW_MOD_SHIFT);

				auto time = glfwGetTime();

				if (m_cursorPosition == g_lastClickPos && time - g_lastClickTime <= DOUBLE_CLICK_TIME) {
					++g_clickCount;
				}
				else {
					g_clickCount = 0;
				}

				g_lastClickTime = time;
				g_lastClickPos = m_cursorPosition;

				switch (g_clickCount % 4) {
					// Highlight Current Word
					case 1:
						cursor_move_to_prev_word(false);
						cursor_move_to_next_word(true);
						break;
					// Highlight Current Line
					case 2:
						cursor_move_to_line_start(false);
						cursor_move_to_line_end(true);
						break;
					// Highlight Whole Text
					case 3:
						cursor_move_to_text_start(false);
						cursor_move_to_text_end(true);
						break;
					default:
						break;
				}
			}
			else {
				release_focus();
			}
		}
		else {
			capture_focus();
			cursor_move_to_mouse(mouseX - get_position()[0], mouseY - get_position()[1], mods & GLFW_MOD_SHIFT);
		}

		g_isMouseDown = true;

		return mouseInside;
	}
	else if (action == GLFW_RELEASE) {
		if (g_focusedTextBox == this) {
			g_isMouseDown = false;
		}
	}

	return false;
}

bool TextBox::handle_key_press(int key, int action, int mods) {
	if (action == GLFW_RELEASE) {
		return false;
	}

	if (is_focused()) {
		bool selectionMode = mods & GLFW_MOD_SHIFT;

		switch (key) {
			case GLFW_KEY_UP:
				cursor_move_to_prev_line(selectionMode);
				return true;
			case GLFW_KEY_DOWN:
				cursor_move_to_next_line(selectionMode);
				return true;
			case GLFW_KEY_LEFT:
				if (mods & GLFW_MOD_CONTROL) {
					cursor_move_to_prev_word(selectionMode);
				}
				else {
					cursor_move_to_prev_character(selectionMode);
				}
				return true;
			case GLFW_KEY_RIGHT:
				if (mods & GLFW_MOD_CONTROL) {
					cursor_move_to_next_word(selectionMode);
				}
				else {
					cursor_move_to_next_character(selectionMode);
				}
				return true;
			case GLFW_KEY_HOME:
				if (mods & GLFW_MOD_CONTROL) {
					cursor_move_to_text_start(selectionMode);
				}
				else {
					cursor_move_to_line_start(selectionMode);
				}
				break;
			case GLFW_KEY_END:
				if (mods & GLFW_MOD_CONTROL) {
					cursor_move_to_text_end(selectionMode);
				}
				else {
					cursor_move_to_line_end(selectionMode);
				}
				break;
			case GLFW_KEY_BACKSPACE:
				handle_key_backspace(mods & GLFW_MOD_CONTROL);
				break;
			case GLFW_KEY_DELETE:
				handle_key_delete(mods & GLFW_MOD_CONTROL);
				break;
			case GLFW_KEY_ENTER:
				handle_key_enter();
				break;
			case GLFW_KEY_X:
				if (mods & GLFW_MOD_CONTROL) {
					clipboard_cut_text();
				}
				break;
			case GLFW_KEY_C:
				if (mods & GLFW_MOD_CONTROL) {
					clipboard_copy_text();
				}
				break;
			case GLFW_KEY_V:
				if (mods & GLFW_MOD_CONTROL) {
					clipboard_paste_text();
				}
				break;
			case GLFW_KEY_A:
				if (mods & GLFW_MOD_CONTROL) {
					cursor_move_to_text_start(false);
					cursor_move_to_text_end(true);
				}
				break;
			default:
				break;
		}

		return true;
	}

	return false;
}

bool TextBox::handle_mouse_move(double mouseX, double mouseY) {
	if (g_focusedTextBox == this && g_isMouseDown) {
		cursor_move_to_mouse(mouseX - get_position()[0], mouseY - get_position()[1], true);
	}

	return false;
}

bool TextBox::handle_text_input(unsigned codepoint) {
	if (g_focusedTextBox == this && m_editable) {
		if (m_selectionStart.is_valid()) {
			remove_highlighted_text();
		}

		int32_t len{};
		char buffer[4]{};
		U8_APPEND_UNSAFE(buffer, len, codepoint);
		insert_text(std::string(buffer, len), m_cursorPosition.get_position());
		return true;
	}

	return false;
}

void TextBox::capture_focus() {
	if (g_focusedTextBox == this) {
		return;
	}
	else if (g_focusedTextBox) {
		g_focusedTextBox->release_focus();
	}

	g_focusedTextBox = this;
	
	recalc_text_internal(should_focused_use_rich_text(), nullptr);
}

void TextBox::release_focus() {
	if (g_focusedTextBox != this) {
		return;
	}

	g_focusedTextBox = nullptr;
	g_isMouseDown = false;
	g_clickCount = 0;
	g_lastClickPos = {CursorPosition::INVALID_VALUE};

	m_selectionStart = {CursorPosition::INVALID_VALUE};

	recalc_text();
}

void TextBox::render(UIContainer& container) {
	bool hasHighlighting = m_selectionStart.is_valid();
	uint32_t selectionStart{};
	uint32_t selectionEnd{};

	// Add highlight ranges in a separate pass to keep from accidental clipping across runs
	if (hasHighlighting) {
		selectionStart = m_selectionStart.get_position();
		selectionEnd = m_cursorPosition.get_position();

		if (selectionStart > selectionEnd) {
			std::swap(selectionStart, selectionEnd);
		}

		m_layout.for_each_run(get_size()[0], m_textXAlignment, [&](auto lineIndex, auto runIndex,
				auto lineX, auto lineY) {
			if (m_layout.run_contains_char_range(runIndex, selectionStart, selectionEnd)) {
				auto [minPos, maxPos] = m_layout.get_position_range_in_run(runIndex, selectionStart,
						selectionEnd);
				
				container.emit_rect(get_position()[0] + lineX + minPos, get_position()[1] + lineY
						- m_layout.get_line_ascent(lineIndex), maxPos - minPos,
						m_layout.get_line_height(lineIndex), Color::from_rgb(0, 120, 215), PipelineIndex::RECT);
			}
		});
	}

	// Draw main text elements
	uint32_t glyphIndex{};
	uint32_t glyphPosIndex{};
	float strikethroughStartPos{};
	float underlineStartPos{};
	auto* glyphPositions = m_layout.get_glyph_position_data();
	m_layout.for_each_run(get_size()[0], m_textXAlignment, [&](auto lineIndex, auto runIndex, auto lineX,
			auto lineY) {
		auto& font = *m_layout.get_run_font(runIndex);

		bool runHasHighlighting = hasHighlighting && m_layout.run_contains_char_range(runIndex,
				selectionStart, selectionEnd);
		Text::Pair<float, float> highlightRange{};
		const Text::Pair<float, float>* pClip = nullptr;

		if (runHasHighlighting) {
			highlightRange = m_layout.get_position_range_in_run(runIndex, selectionStart, selectionEnd);
			highlightRange.first += get_position()[0];
			highlightRange.second += get_position()[0];
			pClip = &highlightRange;
		}

		Text::FormattingIterator iter(m_formatting, m_layout.is_run_rtl(runIndex)
				? m_layout.get_run_char_end_index(runIndex) : m_layout.get_run_char_start_index(runIndex));
		underlineStartPos = strikethroughStartPos = glyphPositions[glyphPosIndex];	

		for (auto glyphEndIndex = m_layout.get_run_glyph_end_index(runIndex); glyphIndex < glyphEndIndex; 
				++glyphIndex, glyphPosIndex += 2) {
			auto pX = glyphPositions[glyphPosIndex];
			auto pY = glyphPositions[glyphPosIndex + 1];
			auto glyphID = m_layout.get_glyph_id(glyphIndex);
			auto event = iter.advance_to(m_layout.get_char_index(glyphIndex));
			auto stroke = iter.get_stroke_state();

			float offset[2]{};
			float texCoordExtents[4]{};
			float glyphSize[2]{};
			bool glyphHasColor{};

			// Stroke
			if (stroke.color.a > 0.f) {
				float offset[2]{};
				float texCoordExtents[4]{};
				float glyphSize[2]{};
				bool strokeHasColor{};
				auto* pGlyphImage = CVars::useMSDF ? g_msdfTextAtlas->get_stroke_info(font, glyphID,
								stroke.thickness, stroke.joins, texCoordExtents, glyphSize, offset,
								strokeHasColor)
						: g_textAtlas->get_stroke_info(font, glyphID, stroke.thickness, stroke.joins,
								texCoordExtents, glyphSize, offset, strokeHasColor);

				container.emit_rect(get_position()[0] + lineX + pX + offset[0],
						get_position()[1] + lineY + pY + offset[1],
						glyphSize[0], glyphSize[1], texCoordExtents, pGlyphImage, stroke.color,
						CVars::useMSDF ? PipelineIndex::MSDF : PipelineIndex::RECT);
			}

			// Main Glyph
			auto* pGlyphImage = CVars::useMSDF ? g_msdfTextAtlas->get_glyph_info(font, glyphID, texCoordExtents,
					glyphSize, offset, glyphHasColor)
					: g_textAtlas->get_glyph_info(font, glyphID, texCoordExtents, glyphSize, offset,
					glyphHasColor);
			auto textColor = glyphHasColor ? Color{1.f, 1.f, 1.f, 1.f} : iter.get_color();

			container.emit_rect(get_position()[0] + lineX + pX + offset[0],
					get_position()[1] + lineY + pY + offset[1], glyphSize[0],
					glyphSize[1], texCoordExtents, pGlyphImage, textColor,
					CVars::useMSDF ? PipelineIndex::MSDF : PipelineIndex::RECT, pClip);
			
			// Underline
			if ((event & Text::FormattingEvent::UNDERLINE_END) != Text::FormattingEvent::NONE) {
				auto height = font.get_underline_thickness() + 0.5f;
				container.emit_rect(get_position()[0] + lineX + underlineStartPos,
						get_position()[1] + lineY + font.get_underline_position(),
						pX - underlineStartPos, height, iter.get_prev_color(), PipelineIndex::RECT, pClip);
			}

			if ((event & Text::FormattingEvent::UNDERLINE_BEGIN) != Text::FormattingEvent::NONE) {
				underlineStartPos = pX;
			}

			// Strikethrough
			if ((event & Text::FormattingEvent::STRIKETHROUGH_END) != Text::FormattingEvent::NONE) {
				auto height = font.get_strikethrough_thickness() + 0.5f;
				container.emit_rect(get_position()[0] + lineX + strikethroughStartPos,
						get_position()[1] + lineY + font.get_strikethrough_position(),
						pX - strikethroughStartPos, height, iter.get_prev_color(), PipelineIndex::RECT, pClip);
			}

			if ((event & Text::FormattingEvent::STRIKETHROUGH_BEGIN) != Text::FormattingEvent::NONE) {
				strikethroughStartPos = pX;
			}
		}

		// Finalize last strikethrough
		if (iter.has_strikethrough()) {
			auto strikethroughEndPos = glyphPositions[glyphPosIndex];
			auto height = font.get_strikethrough_thickness() + 0.5f;
			container.emit_rect(get_position()[0] + lineX + strikethroughStartPos,
					get_position()[1] + lineY + font.get_strikethrough_position(),
					strikethroughEndPos - strikethroughStartPos, height, iter.get_color(), PipelineIndex::RECT,
					pClip);
		}

		// Finalize last underline
		if (iter.has_underline()) {
			auto underlineEndPos = glyphPositions[glyphPosIndex];
			auto height = font.get_underline_thickness() + 0.5f;
			container.emit_rect(get_position()[0] + lineX + underlineStartPos,
					get_position()[1] + lineY + font.get_underline_position(),
					underlineEndPos - underlineStartPos, height, iter.get_color(), PipelineIndex::RECT, pClip);
		}

		glyphPosIndex += 2;
	});

	// Debug render run outlines
	if (CVars::showRunOutlines) {
		m_layout.for_each_run(get_size()[0], m_textXAlignment, [&](auto lineIndex, auto runIndex,
				auto lineX, auto lineY) {
			auto* positions = m_layout.get_run_positions(runIndex);
			auto minBound = positions[0];
			auto maxBound = positions[2 * m_layout.get_run_glyph_count(runIndex)]; 
			container.emit_rect(get_position()[0] + lineX + minBound,
					get_position()[1] + lineY - m_layout.get_line_ascent(lineIndex), maxBound - minBound,
					m_layout.get_line_height(lineIndex), {0, 0.5f, 0, 1}, PipelineIndex::OUTLINE);
		});
	}

	// Debug render glyph boundaries
	if (CVars::showGlyphBoundaries) {
		m_layout.for_each_run(get_size()[0], m_textXAlignment, [&](auto lineIndex, auto runIndex,
				auto lineX, auto lineY) {
			auto* positions = m_layout.get_run_positions(runIndex);

			for (le_int32 i = 0; i <= m_layout.get_run_glyph_count(runIndex); ++i) {
				container.emit_rect(get_position()[0] + lineX + positions[2 * i],
						get_position()[1] + lineY - m_layout.get_line_ascent(lineIndex), 0.5f,
						m_layout.get_line_height(lineIndex), {0, 0.5f, 0, 1}, PipelineIndex::OUTLINE);
			}
		});
	}

	// Draw Cursor
	if (is_focused()) {
		float cursorExtents[] = {get_position()[0] + g_cursorPos.x, get_position()[1] + g_cursorPos.y, 1,
				g_cursorPos.height};
		Color cursorColor{0, 0, 0, 1};

		container.emit_rect(get_position()[0] + g_cursorPos.x, get_position()[1] + g_cursorPos.y, 1,
				g_cursorPos.height, cursorColor, PipelineIndex::RECT);
	}
}

bool TextBox::is_focused() const {
	return g_focusedTextBox == this;
}

// Private Methods

bool TextBox::should_focused_use_rich_text() const {
	// Focused should only use rich text if the text box is not editable
	// NOTE: In a more general sense, this is only true whenever the formatting source is inline
	return m_richText && !m_editable;
}

void TextBox::cursor_move_to_next_character(bool selectionMode) {
	set_cursor_position_internal(g_cursorCtrl.next_character(m_cursorPosition), selectionMode);
	recalc_text_internal(should_focused_use_rich_text(), nullptr);
}

void TextBox::cursor_move_to_prev_character(bool selectionMode) {
	set_cursor_position_internal(g_cursorCtrl.prev_character(m_cursorPosition), selectionMode);
	recalc_text_internal(should_focused_use_rich_text(), nullptr);
}

void TextBox::cursor_move_to_next_word(bool selectionMode) {
	set_cursor_position_internal(g_cursorCtrl.next_word(m_cursorPosition), selectionMode);
	recalc_text_internal(should_focused_use_rich_text(), nullptr);
}

void TextBox::cursor_move_to_prev_word(bool selectionMode) {
	set_cursor_position_internal(g_cursorCtrl.prev_word(m_cursorPosition), selectionMode);
	recalc_text_internal(should_focused_use_rich_text(), nullptr);
}

void TextBox::cursor_move_to_next_line(bool selectionMode) {
	PostLayoutCursorMove op{PostLayoutCursorMoveType::LINE_BELOW, selectionMode};
	recalc_text_internal(should_focused_use_rich_text(), &op);
}

void TextBox::cursor_move_to_prev_line(bool selectionMode) {
	PostLayoutCursorMove op{PostLayoutCursorMoveType::LINE_ABOVE, selectionMode};
	recalc_text_internal(should_focused_use_rich_text(), &op);
}

void TextBox::cursor_move_to_line_start(bool selectionMode) {
	PostLayoutCursorMove op{PostLayoutCursorMoveType::LINE_START, selectionMode};
	recalc_text_internal(should_focused_use_rich_text(), &op);
}

void TextBox::cursor_move_to_line_end(bool selectionMode) {
	PostLayoutCursorMove op{PostLayoutCursorMoveType::LINE_END, selectionMode};
	recalc_text_internal(should_focused_use_rich_text(), &op);
}

void TextBox::cursor_move_to_text_start(bool selectionMode) {
	set_cursor_position_internal({}, selectionMode);
	recalc_text_internal(should_focused_use_rich_text(), nullptr);
}

void TextBox::cursor_move_to_text_end(bool selectionMode) {
	set_cursor_position_internal({static_cast<uint32_t>(m_text.size())}, selectionMode);
	recalc_text_internal(should_focused_use_rich_text(), nullptr);
}

void TextBox::cursor_move_to_mouse(double mouseX, double mouseY, bool selectionMode) {
	CursorToMouse op{
		.mouseX = mouseX,
		.mouseY = mouseY,
	};
	op.type = PostLayoutCursorMoveType::MOUSE_POSITION;
	op.selectionMode = selectionMode;
	recalc_text_internal(should_focused_use_rich_text(), &op);
}

void TextBox::set_cursor_position_internal(CursorPosition pos, bool selectionMode) {
	if (selectionMode) {
		if (!m_selectionStart.is_valid()) {
			m_selectionStart = m_cursorPosition;
		}

		m_cursorPosition = pos;
	}
	else {
		m_selectionStart = {CursorPosition::INVALID_VALUE};
		m_cursorPosition = pos;
	}
}

void TextBox::handle_key_backspace(bool ctrl) {
	if (m_selectionStart.is_valid()) {
		remove_highlighted_text();
	}
	else if (m_cursorPosition.get_position() > 0) {
		auto endPos = m_cursorPosition.get_position();

		if (ctrl) {
			cursor_move_to_prev_word(false);
		}
		else {
			cursor_move_to_prev_character(false);
		}

		remove_text(m_cursorPosition.get_position(), endPos);
	}
}

void TextBox::handle_key_delete(bool ctrl) {
	if (m_selectionStart.is_valid()) {
		remove_highlighted_text();
	}
	else if (m_cursorPosition.get_position() < m_text.size()) {
		auto startPos = m_cursorPosition;

		if (ctrl) {
			cursor_move_to_next_word(false);
		}
		else {
			cursor_move_to_next_character(false);
		}

		auto endPos = m_cursorPosition.get_position();
		m_cursorPosition = startPos;
		remove_text(startPos.get_position(), endPos);
	}
}

void TextBox::handle_key_enter() {
	if (m_multiLine) {
		remove_highlighted_text();
		insert_text("\n", m_cursorPosition.get_position());
	}
	else {
		release_focus();
	}
}

void TextBox::clipboard_cut_text() {
	if (!m_editable) {
		return;
	}

	clipboard_copy_text();
	remove_highlighted_text();
}

void TextBox::clipboard_copy_text() {
	if (!m_selectionStart.is_valid()) {
		return;
	}

	auto startPos = m_selectionStart.get_position();
	auto endPos = m_cursorPosition.get_position();

	if (startPos == endPos) {
		return;
	}
	else if (startPos > endPos) {
		std::swap(startPos, endPos);
	}

	auto str = m_text.substr(startPos, endPos - startPos);
	glfwSetClipboardString(NULL, str.c_str());
}

void TextBox::clipboard_paste_text() {
	if (!m_editable) {
		return;
	}

	remove_highlighted_text();
	insert_text(glfwGetClipboardString(NULL), m_cursorPosition.get_position());
}

void TextBox::insert_text(const std::string& text, uint32_t startIndex) {
	m_cursorPosition = {static_cast<uint32_t>(m_cursorPosition.get_position() + text.size())};

	if (startIndex < m_text.size()) {
		auto before = m_text.substr(0, startIndex);
		auto after = m_text.substr(startIndex);
		set_text(before + text + after);
	}
	else {
		set_text(m_text + text);
	}
}

void TextBox::remove_text(uint32_t startIndex, uint32_t endIndex) {
	auto before = m_text.substr(0, startIndex);
	auto after = m_text.substr(endIndex);
	set_text(before + after);
}

void TextBox::remove_highlighted_text() {
	auto start = m_selectionStart;
	auto end = m_cursorPosition;

	if (start == end || !start.is_valid()) {
		return;
	}
	else if (start.get_position() > end.get_position()) {
		std::swap(start, end);
	}

	m_cursorPosition = start;
	m_selectionStart = {CursorPosition::INVALID_VALUE};
	remove_text(start.get_position(), end.get_position());
}

void TextBox::recalc_text() {
	recalc_text_internal(is_focused() ? should_focused_use_rich_text() : m_richText, nullptr);
}

void TextBox::recalc_text_internal(bool richText, const void* postLayoutOp) {
	g_cursorPos.x = 0.f;
	g_cursorPos.y = 0.f;
	g_cursorPos.height = 0.f;
	g_cursorPos.lineNumber = 0;

	if (!m_font) {
		return;
	}

	Text::StrokeState strokeState{};
	m_formatting = richText
			? Text::parse_inline_formatting(m_text, m_contentText, m_font, m_textColor, strokeState)
			: Text::make_default_formatting_runs(m_text, m_contentText, m_font, m_textColor, strokeState);

	if (m_contentText.empty()) {
		g_cursorPos.height = static_cast<float>(m_font.getAscent() + m_font.getDescent());
		return;
	}

	if (g_focusedTextBox == this) {
		g_cursorCtrl.set_text(should_focused_use_rich_text() ? m_contentText : m_text);
	}

	auto& text = richText ? m_contentText : m_text;
	Text::build_layout_info_utf8(m_layout, text.data(), text.size(), m_formatting.fontRuns,
			m_textWrapped ? get_size()[0] : 0.f, get_size()[1], m_textYAlignment, Text::LayoutInfoFlags::NONE);

	if (postLayoutOp) {
		set_cursor_position_internal(apply_cursor_move(m_layout, get_size()[0], m_textXAlignment,
				*reinterpret_cast<const PostLayoutCursorMove*>(postLayoutOp), m_cursorPosition),
				reinterpret_cast<const PostLayoutCursorMove*>(postLayoutOp)->selectionMode);
	}

	g_cursorPos = m_layout.calc_cursor_pixel_pos(get_size()[0], m_textXAlignment, m_cursorPosition);
}

// Setters

void TextBox::set_font(MultiScriptFont font) {
	m_font = std::move(font);
	recalc_text();
}

void TextBox::set_text(std::string text) {
	m_text = std::move(text);
	recalc_text();
}

void TextBox::set_text_x_alignment(TextXAlignment align) {
	m_textXAlignment = align;
	recalc_text();
}

void TextBox::set_text_y_alignment(TextYAlignment align) {
	m_textYAlignment = align;
	recalc_text();
}

void TextBox::set_text_wrapped(bool wrapped) {
	m_textWrapped = wrapped;
	recalc_text();
}

void TextBox::set_multi_line(bool multiLine) {
	m_multiLine = multiLine;
}

void TextBox::set_rich_text(bool richText) {
	m_richText = richText;
	recalc_text();
}

void TextBox::set_editable(bool editable) {
	m_editable = editable;
}

void TextBox::set_selectable(bool selectable) {
	m_selectable = selectable;
}

// Static Functions

static CursorPosition apply_cursor_move(const Text::LayoutInfo& layout, float textWidth,
		TextXAlignment textXAlignment, const PostLayoutCursorMove& op, CursorPosition cursor) {
	switch (op.type) {
		case PostLayoutCursorMoveType::LINE_START:
			return layout.get_line_start_position(g_cursorPos.lineNumber);
		case PostLayoutCursorMoveType::LINE_END:
			return layout.get_line_end_position(g_cursorPos.lineNumber);
		case PostLayoutCursorMoveType::LINE_ABOVE:
			return g_cursorPos.lineNumber > 0
					? g_cursorCtrl.closest_in_line(layout, textWidth, textXAlignment, g_cursorPos.lineNumber - 1,
							g_cursorPos.x)
					: cursor;
		case PostLayoutCursorMoveType::LINE_BELOW:
			return g_cursorPos.lineNumber < layout.get_line_count() - 1
					? g_cursorCtrl.closest_in_line(layout, textWidth, textXAlignment, g_cursorPos.lineNumber + 1,
							g_cursorPos.x)
					: cursor;
		case PostLayoutCursorMoveType::MOUSE_POSITION:
		{
			auto& mouseOp = static_cast<const CursorToMouse&>(op);
			return g_cursorCtrl.closest_to_position(layout, textWidth, textXAlignment,
					static_cast<float>(mouseOp.mouseX), static_cast<float>(mouseOp.mouseY));
		}
			break;
		default:
			break;
	}

	return cursor;
}

