/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	spell_tessronos.cc
 *	ブラウザ: the spelling of what is typed into a page's fields (design
 *	14.4.6): the frames' text check client, on hunspell with the English
 *	dictionary linked into the program (tools/browser/spell, en_US).
 *
 *	Blink asks for the text of the field being edited to be checked
 *	(RequestCheckingOfText), and of one word when a misspelling's menu
 *	is wanted (CheckSpelling). The text is cut into words of Latin
 *	letters and apostrophes; a word with a digit or of other letters (the
 *	Japanese of a field among them) is passed over. A word hunspell does
 *	not know is marked, with up to three suggestions. The dictionary is
 *	read the first time a word is checked.
 */

#include <stdio.h>
#include <string.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_text_check_client.h"
#include "third_party/blink/public/web/web_text_checking_completion.h"
#include "third_party/blink/public/web/web_text_checking_result.h"
#include "third_party/hunspell/src/hunspell/hunspell.hxx"

extern "C" const char br_spell_aff[], br_spell_aff_end[];	/* tools/browser/build.sh */
extern "C" const char br_spell_dic[], br_spell_dic_end[];

/* The bytes of a dictionary's file by its name (port/spell/filemgr_tessronos.cc) */
bool br_spell_file( const char *name, const char **data, size_t *len )
{
	if ( strcmp(name, "en_US.aff") == 0 ) {
		*data = br_spell_aff;
		*len = (size_t)( br_spell_aff_end - br_spell_aff );
		return true;
	}
	if ( strcmp(name, "en_US.dic") == 0 ) {
		*data = br_spell_dic;
		*len = (size_t)( br_spell_dic_end - br_spell_dic );
		return true;
	}
	return false;
}

namespace {

constexpr int kSuggestions = 3;

std::once_flag			g_once;
std::unique_ptr<Hunspell>	g_hun;

Hunspell *speller()
{
	std::call_once(g_once, [] {
		g_hun = std::make_unique<Hunspell>("en_US.aff", "en_US.dic");
		printf("browser: spelling: the English dictionary read\n");
	});
	return g_hun.get();
}

bool latin( char16_t c )
{
	return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7 );
}

bool digit( char16_t c )
{
	return c >= '0' && c <= '9';
}

/* A word not in the dictionary: its place in the text, and what it may have been */
struct Miss {
	size_t				at, len;
	std::vector<blink::WebString>	sugg;
};

std::vector<Miss> check( const std::u16string &text, bool suggest )
{
	std::vector<Miss> out;
	Hunspell *h = speller();
	size_t i = 0, n = text.size();

	while ( i < n ) {
		if ( !latin(text[i]) && !digit(text[i]) ) {
			i++;
			continue;
		}
		size_t s = i;
		bool other = false;
		while ( i < n && ( latin(text[i]) || digit(text[i]) || text[i] == '\'' || text[i] == 0x2019
				   || text[i] >= 0x3000 ) ) {
			if ( digit(text[i]) || text[i] >= 0x3000 ) other = true;
			i++;
		}
		size_t e = i;
		while ( e > s && ( text[e - 1] == '\'' || text[e - 1] == 0x2019 ) ) e--;
		if ( other || e <= s + 1 ) continue;
		std::string word = base::UTF16ToUTF8(text.substr(s, e - s));
		if ( h->spell(word) ) continue;
		Miss m{s, e - s, {}};
		if ( suggest ) {
			std::vector<std::string> list = h->suggest(word);
			for ( size_t k = 0; k < list.size() && (int)k < kSuggestions; k++ ) {
				m.sugg.push_back(blink::WebString::FromUtf8(list[k]));
			}
		}
		out.push_back(std::move(m));
	}
	return out;
}

class Spelling final : public blink::WebTextCheckClient {
public:
	bool IsSpellCheckingEnabled() const override { return true; }

	void CheckSpelling( const blink::WebString &text, size_t &at, size_t &len,
			    std::vector<blink::WebString> *suggestions ) override
	{
		std::vector<Miss> m = check(text.Utf16(), suggestions != nullptr);
		at = len = 0;
		if ( m.empty() ) return;
		at = m[0].at;
		len = m[0].len;
		if ( suggestions != nullptr ) *suggestions = m[0].sugg;
	}

	void RequestCheckingOfText( const blink::WebString &text, const std::vector<blink::WebSpellingMarker> &,
				    ShouldForceRefreshTextCheckService,
				    std::unique_ptr<blink::WebTextCheckingCompletion> done ) override
	{
		std::vector<blink::WebTextCheckingResult> results;
		for ( Miss &m : check(text.Utf16(), true) ) {
			results.emplace_back(blink::kWebTextDecorationTypeSpelling, (int)m.at, (int)m.len, m.sugg);
		}
		done->DidFinishCheckingText(results);
	}
};

}  // namespace

/* The text check client every frame is given */
blink::WebTextCheckClient *br_spell_client()
{
	static Spelling *client = new Spelling();
	return client;
}
