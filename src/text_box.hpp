#pragma once

#include "bitmap.hpp"
#include "multi_script_font.hpp"
#include "pipeline.hpp"

#include <string>
#include <vector>

namespace RichText { struct Result; }
namespace RichText { template <typename> class TextRuns; }

class Image;
class Pipeline;

struct TextRect {
	float x;
	float y;
	float width;
	float height;
	float texCoords[4];
	Image* texture;
	Color color;
	PipelineIndex pipeline;
};

class TextBox {
	public:
		void render(const float* invScreenSize);

		void set_font(MultiScriptFont);
		void set_text(std::string);
		void set_position(float x, float y);
		void set_size(float width, float height);
		void set_text_wrapped(bool);
		void set_rich_text(bool);
	private:
		MultiScriptFont m_font{};
		float m_position[2]{};
		float m_size[2]{};
		std::string m_text{};
		std::string m_contentText{};
		Color m_textColor{0.f, 0.f, 0.f, 1.f};
		bool m_textWrapped = false;
		bool m_richText = false;

		std::vector<TextRect> m_textRects;

		void recalc_text();
		void create_text_rects(RichText::Result&);
};

