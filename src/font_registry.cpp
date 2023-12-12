#include "font_registry.hpp"

#include "string_hash.hpp"
#include "file_read_bytes.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <hb-ft.h>

#include <unicode/utext.h>

#include <cassert>
#include <cstring>

#include <bitset>
#include <unordered_map>
#include <vector>

using namespace Text;

static constexpr const size_t WEIGHT_COUNT = static_cast<size_t>(FontWeight::COUNT);
static constexpr const size_t STYLE_COUNT = static_cast<size_t>(FontStyle::COUNT);

namespace {

struct FaceData {
	std::string name;
	std::vector<char> fileData;
};

struct FamilyData {
	FontFace lookup[WEIGHT_COUNT][STYLE_COUNT]{};
	std::vector<FontFamily> linkedFamilies;
	std::vector<FontFamily> fallbackFamilies;
	std::bitset<USCRIPT_CODE_LIMIT> scripts;
	bool initialized{};

	FontFace get_face(FontWeight weight, FontStyle style) const {
		return lookup[static_cast<size_t>(weight)][static_cast<size_t>(style)];
	}

	bool has_script(UScriptCode script) const {
		return scripts.test(static_cast<size_t>(script));
	}
};

struct FontDataOwner {
	FT_Face ftFace{};
	hb_font_t* hbFont{};
	uint32_t size{};
	int16_t strikethroughPosition;
	int16_t strikethroughThickness;

	FontDataOwner() = default;
	explicit FontDataOwner(FT_Face ftFaceIn, hb_font_t* hbFontIn, uint32_t sizeIn, int16_t strikePosIn,
				int16_t strikeThickIn)
			: ftFace(ftFaceIn)
			, hbFont(hbFontIn)
			, size(sizeIn)
			, strikethroughPosition(strikePosIn)
			, strikethroughThickness(strikeThickIn) {}
	explicit FontDataOwner(const FontData& fontData, uint32_t sizeIn)
			: ftFace(fontData.ftFace)
			, hbFont(fontData.hbFont)
			, strikethroughPosition(fontData.strikethroughPosition)
			, strikethroughThickness(fontData.strikethroughThickness)
			, size(sizeIn) {}

	FontDataOwner(FontDataOwner&& other) noexcept {
		*this = std::move(other);
	}

	FontDataOwner& operator=(FontDataOwner&& other) noexcept {
		std::swap(ftFace, other.ftFace);
		std::swap(hbFont, other.hbFont);
		strikethroughPosition = other.strikethroughPosition;
		strikethroughThickness = other.strikethroughThickness;
		return *this;
	}

	FontDataOwner(const FontDataOwner&) = delete;
	void operator=(const FontDataOwner&) = delete;

	~FontDataOwner() {
		if (ftFace) {
			FT_Done_Face(ftFace);
		}

		if (hbFont) {
			hb_font_destroy(hbFont);
		}
	}

	operator FontData() const {
		return {ftFace, hbFont, strikethroughPosition, strikethroughThickness};
	}

	void resize(uint32_t newSize) {
		if (size == newSize) {
			return;
		}

		size = newSize;

		FT_Size_RequestRec sr{
			.type = FT_SIZE_REQUEST_TYPE_REAL_DIM,
			.height = static_cast<FT_Long>(size) * 64,
		};
		FT_Request_Size(ftFace, &sr);
		hb_ft_font_changed(hbFont);
	}
};

struct FontContext {
	FT_Library lib;
	std::unordered_map<FaceIndex_T, FontDataOwner> cache;

	explicit FontContext() {
		FT_Init_FreeType(&lib);
	}

	~FontContext() {
		cache.clear();
		FT_Done_FreeType(lib);
	}
};

}

thread_local FontContext t_fontContext;

static std::vector<FaceData> g_faces;
static std::unordered_map<std::string, FontFace, StringHash, std::equal_to<>> g_facesByName;

static std::vector<FamilyData> g_familyData;
static std::unordered_map<std::string, FontFamily, StringHash, std::equal_to<>> g_familiesByName;

static bool family_is_initialized(FontFamily family);
static std::bitset<USCRIPT_CODE_LIMIT>& family_get_scripts(FontFamily family);
static std::vector<FontFamily>& family_get_linked(FontFamily family);
static std::vector<FontFamily>& family_get_fallback(FontFamily family);

static SingleScriptFont get_sub_font(Text::Font font, UText& iter, int32_t& offset, int32_t limit,
		UScriptCode script);

static FontFamily get_or_add_family(const std::string_view& name);
static FontFace get_or_add_face(const FontFaceCreateInfo& faceInfo);

static FontFace get_font_for_script(FontFamily family, FontWeight weight, FontStyle style,
		UScriptCode script);
static FontFace find_compatible_font(Text::Font font, uint32_t codepoint, FontFace baseFont,
		const std::vector<FontFamily>& fallbackFamilies, FontData& fontData);

// Public Functions

FontFamily FontRegistry::get_family(std::string_view name) {
	if (auto it = g_familiesByName.find(name); it != g_familiesByName.end()) {
		return {it->second};
	}

	return {};
}

FontFace FontRegistry::get_face(Font font) {
	assert(font && "FontRegistry::get_face must be called with a valid font");
	return g_familyData[font.get_family().handle].get_face(font.get_weight(), font.get_style());
}

FontData FontRegistry::get_font_data(Font font) {
	assert(font.valid() && "get_font_data(): Must pass valid Font");
	assert(font.get_family().valid() && "get_font_data(): Must pass valid FontFamily");
	assert(family_is_initialized(font.get_family()) && "get_font_data(): Must pass initialized FontFamily");
	return get_font_data(get_face(font), font.get_size());
}

FontData FontRegistry::get_font_data(SingleScriptFont font) {
	return get_font_data(font.face, font.size);
}

FontData FontRegistry::get_font_data(FontFace face, uint32_t size) {
	if (auto it = t_fontContext.cache.find(face.handle); it != t_fontContext.cache.end()) {
		it->second.resize(size);
		return it->second;
	}

	assert(face.valid() && "get_font_data(): Must pass valid face");
	assert(size > 0 && "get_font_data(): Must pass valid size");

	FontData fontData{};
	auto& faceData = g_faces[face.handle];

	if (FT_New_Memory_Face(t_fontContext.lib, reinterpret_cast<const FT_Byte*>(faceData.fileData.data()),
			faceData.fileData.size(), 0, &fontData.ftFace) != 0) {
		return {};
	}

	fontData.hbFont = hb_ft_font_create(fontData.ftFace, nullptr);

	if (!fontData.hbFont) {
		return {};
	}

	FT_Size_RequestRec sr{
		.type = FT_SIZE_REQUEST_TYPE_REAL_DIM,
		.height = static_cast<FT_Long>(size) * 64,
	};
	FT_Request_Size(fontData.ftFace, &sr);

	hb_ft_font_set_load_flags(fontData.hbFont, FT_LOAD_DEFAULT);

	if (auto* pOS2Table = reinterpret_cast<TT_OS2*>(FT_Get_Sfnt_Table(fontData.ftFace, FT_SFNT_OS2))) {
		fontData.strikethroughPosition = -pOS2Table->yStrikeoutPosition;
		fontData.strikethroughThickness = pOS2Table->yStrikeoutSize;
	}

	t_fontContext.cache.emplace(std::make_pair(face.handle, FontDataOwner(fontData, size)));

	return fontData;
}

FontRegistryError FontRegistry::register_family(const FontFamilyCreateInfo& familyInfo) {
	auto family = get_or_add_family(familyInfo.name);

	if (family_is_initialized(family)) {
		return FontRegistryError::ALREADY_LOADED;
	}

	// Initialize scripts
	if (familyInfo.scriptCodeCount > 0) {
		for (uint32_t i = 0; i < familyInfo.scriptCodeCount; ++i) {
			family_get_scripts(family).set(familyInfo.pScriptCodes[i]);
		}
	}
	else {
		family_get_scripts(family).set();
	}

	// Initialize linked families
	family_get_linked(family).reserve(familyInfo.linkedFamilyCount);

	for (uint32_t i = 0; i < familyInfo.linkedFamilyCount; ++i) {
		auto linked = get_or_add_family(familyInfo.pLinkedFamilies[i]);
		family_get_linked(family).emplace_back(linked);
	}

	// Initialize fallback families
	family_get_fallback(family).reserve(familyInfo.fallbackFamilyCount);

	for (uint32_t i = 0; i < familyInfo.fallbackFamilyCount; ++i) {
		auto fallback = get_or_add_family(familyInfo.pFallbackFamilies[i]);
		family_get_fallback(family).emplace_back(fallback);
	}

	if (!familyInfo.pFaces) {
		family_get_scripts(family).reset();
		family_get_linked(family).clear();
		family_get_fallback(family).clear();
		return FontRegistryError::NO_FACES;
	}

	auto& faceLookup = g_familyData[family.handle].lookup;
	FontFace defaultFace{};

	for (uint32_t i = 0; i < familyInfo.faceCount; ++i) {
		auto& faceInfo = familyInfo.pFaces[i];
		auto face = get_or_add_face(faceInfo);
		faceLookup[static_cast<size_t>(faceInfo.weight)][static_cast<size_t>(faceInfo.style)] = face;

		if (face) {
			// Find a default face among provided faces; prefer Regular/Normal
			if (faceInfo.weight == FontWeight::REGULAR && faceInfo.style == FontStyle::NORMAL) {
				defaultFace = face;
			}
			else if (!defaultFace) {
				defaultFace = face;
			}
		}
	}

	// Apply default face to missing faces
	for (size_t weight = 0; weight < WEIGHT_COUNT; ++weight) {
		for (size_t style = 0; style < STYLE_COUNT; ++style) {
			if (!faceLookup[weight][style]) {
				faceLookup[weight][style] = defaultFace;
			}
		}
	}

	g_familyData[family.handle].initialized = true;
	return FontRegistryError::NONE;
}

SingleScriptFont FontRegistry::get_sub_font(Text::Font font, const char* text, int32_t& offset, int32_t limit,
		UScriptCode script) {
	UText iter UTEXT_INITIALIZER;
	UErrorCode errc{};
	utext_openUTF8(&iter, text + offset, limit - offset, &errc);
	return get_sub_font(font, iter, offset, limit, script);
}

SingleScriptFont FontRegistry::get_sub_font(Text::Font font, const char16_t* text, int32_t& offset, 
		int32_t limit, UScriptCode script) {
	UText iter UTEXT_INITIALIZER;
	UErrorCode errc{};
	utext_openUChars(&iter, text + offset, limit - offset, &errc);
	return get_sub_font(font, iter, offset, limit, script);
}

// Static Functions

static bool family_is_initialized(FontFamily family) {
	return g_familyData[family.handle].initialized;
}

static std::bitset<USCRIPT_CODE_LIMIT>& family_get_scripts(FontFamily family) {
	return g_familyData[family.handle].scripts;
}

static std::vector<FontFamily>& family_get_linked(FontFamily family) {
	return g_familyData[family.handle].linkedFamilies;
}

static std::vector<FontFamily>& family_get_fallback(FontFamily family) {
	return g_familyData[family.handle].fallbackFamilies;
}

static SingleScriptFont get_sub_font(Text::Font font, UText& iter, int32_t& offset, int32_t limit,
		UScriptCode script) {
	assert(font.valid() && "get_sub_font(): Must be called with a valid font");
	assert(font.get_family().valid() && "get_sub_font(): Must be called with a valid font family");
	assert(family_is_initialized(font.get_family()) && "get_sub_font(): Base family must be initialized");

	auto baseFont = get_font_for_script(font.get_family(), font.get_weight(), font.get_style(), script);
	auto& fallbackFamilies = g_familyData[font.get_family().handle].fallbackFamilies;

	// Find the longest run that the base font or its fallbacks are able to draw

	// First, find the first font that is able to render a char from the string.

	FontFace targetFace{};
	FontData fontData;

	for (;;) {
		auto c = UTEXT_NEXT32(&iter);

		if (c == U_SENTINEL) {
			break;
		}
		else if (auto face = find_compatible_font(font, c, baseFont, fallbackFamilies, fontData)) {
			targetFace = face;
			break;
		}
	}

	// No font can render this substring, just use the base font
	if (!targetFace) {
		offset = limit;
		return {baseFont, font.get_size()};
	}
	
	// Then, see how long it is able to render characters

	for (;;) {
		auto idx = UTEXT_GETNATIVEINDEX(&iter);
		auto c = UTEXT_NEXT32(&iter);

		if (c == U_SENTINEL) {
			break;
		}
		else if (!fontData.has_codepoint(c)) {
			offset = offset + idx;
			return {targetFace, font.get_size()};
		}
	}

	offset = limit;
	return {targetFace, font.get_size()};
}

static FontFamily get_or_add_family(const std::string_view& name) {
	if (auto it = g_familiesByName.find(name); it != g_familiesByName.end()) {
		return it->second;
	}

	FontFamily result{static_cast<FamilyIndex_T>(g_familyData.size())};
	g_familiesByName.emplace(std::make_pair(std::string(name), result));
	g_familyData.emplace_back();

	return result;
}

static FontFace get_or_add_face(const FontFaceCreateInfo& faceInfo) {
	if (auto it = g_facesByName.find(faceInfo.name); it != g_facesByName.end()) {
		return it->second;
	}

	FontFace result{static_cast<FaceIndex_T>(g_faces.size())};
	g_facesByName.emplace(std::make_pair(std::string(faceInfo.name), result));

	g_faces.push_back({
		.name = std::string(faceInfo.name),
		.fileData = file_read_bytes(std::string(faceInfo.uri).c_str()),
	});

	return result;
}

static FontFace get_font_for_script(FontFamily family, FontWeight weight, FontStyle style,
		UScriptCode script) {
	if (g_familyData[family.handle].has_script(script)) {
		return g_familyData[family.handle].get_face(weight, style);
	}
	else {
		for (auto linkedFamily : g_familyData[family.handle].linkedFamilies) {
			auto& linked = g_familyData[linkedFamily.handle];

			if (family_is_initialized(linkedFamily) && linked.has_script(script)) {
				return linked.get_face(weight, style);
			}
		}
	}

	return g_familyData[family.handle].get_face(weight, style);
}

static FontFace find_compatible_font(Text::Font font, uint32_t codepoint, FontFace baseFont,
		const std::vector<FontFamily>& fallbackFamilies, FontData& fontData) {
	if (!baseFont) {
		return {};
	}
	
	fontData = FontRegistry::get_font_data(baseFont, font.get_size());
	if (!fontData) {
		return {};
	}

	if (fontData.has_codepoint(codepoint)) {
		return baseFont;
	}

	for (auto fam : fallbackFamilies) {
		if (!family_is_initialized(fam)) {
			continue;
		}

		auto face = g_familyData[fam.handle].get_face(font.get_weight(), font.get_style());
		fontData = FontRegistry::get_font_data(face, font.get_size());

		if (!fontData) {
			continue;
		}

		if (fontData.has_codepoint(codepoint)) {
			return face;
		}
	}

	return {};
}

