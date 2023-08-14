function(get_version_parts version out_MAJOR out_MINOR out_PATCH out_TWEAK)
	set(version_REGEX "^[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?(\\.[0-9]+)?$")
	set(version_REGEX_1 "^[0-9]+$")
	set(version_REGEX_2 "^[0-9]+\\.[0-9]+$")
	set(version_REGEX_3 "^[0-9]+\\.[0-9]+\\.[0-9]+$")
	set(version_REGEX_4 "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")

	if (NOT version MATCHES ${version_REGEX})
		message(FATAL_ERROR "Problem parsing version string.")
	endif()

	if (version MATCHES ${version_REGEX_1})
		set(count 1)
	elseif (version MATCHES ${version_REGEX_2})
		set(count 2)
	elseif (version MATCHES ${version_REGEX_3})
		set(count 3)
	elseif (version MATCHES ${version_REGEX_4})
		set(count 4)
	endif()

	string(REGEX REPLACE "^([0-9]+)(\\.[0-9]+)?(\\.[0-9]+)?(\\.[0-9]+)?" "\\1" major "${version}")

	if (NOT count LESS 2)
		string(REGEX REPLACE "^[0-9]+\\.([0-9]+)(\\.[0-9]+)?(\\.[0-9]+)?" "\\1" minor "${version}")
	else()
		set(minor "0")
	endif()

	if (NOT count LESS 3)
		string(REGEX REPLACE "^[0-9]+\\.[0-9]+\\.([0-9]+)(\\.[0-9]+)?" "\\1" patch "${version}")
	else()
		set(patch "0")
	endif()

	if (NOT count LESS 4)
		string(REGEX REPLACE "^[0-9]+\\.[0-9]+\\.[0-9]+\\.([0-9]+)" "\\1" tweak "${version}")
	else()
		set(tweak "0")
	endif()

	set(${out_MAJOR} "${major}" PARENT_SCOPE)
	set(${out_MINOR} "${minor}" PARENT_SCOPE)
	set(${out_PATCH} "${patch}" PARENT_SCOPE)
	set(${out_TWEAK} "${tweak}" PARENT_SCOPE)
endfunction()

function(get_icu_version out_VERSION)
	set(src_FILE ${ICU_SOURCE_DIR}/common/unicode/uvernum.h)
	file(READ ${src_FILE} src_FILE_CONTENTS)
	string(REGEX MATCH "[ \t]*#[ \t]*define[ \t]+U_ICU_VERSION[ \t]+\"([^\"]*)\"" version_str ${src_FILE_CONTENTS})
	if (CMAKE_MATCH_COUNT EQUAL 1)
		set(${out_VERSION} ${CMAKE_MATCH_1} PARENT_SCOPE)
	endif()
endfunction()
