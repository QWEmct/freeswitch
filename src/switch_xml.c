/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005/2006, Anthony Minessale II <anthmct@yahoo.com>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthmct@yahoo.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthmct@yahoo.com>
 * Simon Capper <skyjunky@sbcglobal.net>
 *
 *
 * switch_xml.c -- XML PARSER
 *
 * Derived from ezxml http://ezxml.sourceforge.net
 * Original Copyright
 *
 * Copyright 2004, 2006 Aaron Voisine <aaron@voisine.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <switch.h>
#ifndef WIN32
#include <switch_private.h>
#include <glob.h>
#else /* we're on windoze :( */
/* glob functions at end of this file */
#include <apr_file_io.h>

typedef struct {
	size_t gl_pathc;	/* Count of total paths so far. */
	size_t gl_matchc;	/* Count of paths matching pattern. */
	size_t gl_offs;		/* Reserved at beginning of gl_pathv. */
	int gl_flags;		/* Copy of flags parameter to glob. */
	char **gl_pathv;	/* List of paths matching pattern. */
	/* Copy of errfunc parameter to glob. */
	int (*gl_errfunc)(const char *, int);	
} glob_t;

/* Believed to have been introduced in 1003.2-1992 */
#define	GLOB_APPEND	0x0001	/* Append to output from previous call. */
#define	GLOB_DOOFFS	0x0002	/* Use gl_offs. */
#define	GLOB_ERR	0x0004	/* Return on error. */
#define	GLOB_MARK	0x0008	/* Append / to matching directories. */
#define	GLOB_NOCHECK	0x0010	/* Return pattern itself if nothing matches. */
#define	GLOB_NOSORT	0x0020	/* Don't sort. */

/* Error values returned by glob(3) */
#define	GLOB_NOSPACE	(-1)	/* Malloc call failed. */
#define	GLOB_ABORTED	(-2)	/* Unignored error. */
#define	GLOB_NOMATCH	(-3)	/* No match and GLOB_NOCHECK was not set. */
#define	GLOB_NOSYS	(-4)	/* Obsolete: source comptability only. */

#define	GLOB_ALTDIRFUNC	0x0040	/* Use alternately specified directory funcs. */
#define	GLOB_MAGCHAR	0x0100	/* Pattern had globbing characters. */
#define	GLOB_NOMAGIC	0x0200	/* GLOB_NOCHECK without magic chars (csh). */
#define	GLOB_QUOTE	0x0400	/* Quote special chars with \. */
#define	GLOB_LIMIT	0x1000	/* limit number of returned paths */

int	glob(const char *, int, int (*)(const char *, int), glob_t *);
void	globfree(glob_t *);

#endif
#undef HAVE_MMAP
#ifdef HAVE_MMAP
#include <sys/mman.h>
#ifdef __sun
extern int madvise(caddr_t, size_t, int);
#endif
#endif

#define SWITCH_XML_WS   "\t\r\n "	// whitespace
#define SWITCH_XML_ERRL 128		// maximum error string length

static int preprocess(const char *cwd, const char *file, int write_fd, int rlevel);

typedef struct switch_xml_root *switch_xml_root_t;
struct switch_xml_root {		// additional data for the root tag
	struct switch_xml xml;		// is a super-struct built on top of switch_xml struct
	switch_xml_t cur;			// current xml tree insertion point
	char *m;					// original xml string
	switch_size_t len;			// length of allocated memory for mmap
	uint8_t dynamic;
	char *u;					// UTF-8 conversion of string if original was UTF-16
	char *s;					// start of work area
	char *e;					// end of work area
	char **ent;					// general entities (ampersand sequences)
	char ***attr;				// default attributes
	char ***pi;					// processing instructions
	short standalone;			// non-zero if <?xml standalone="yes"?>
	char err[SWITCH_XML_ERRL];	// error string
};

char *SWITCH_XML_NIL[] = { NULL };	// empty, null terminated array of strings

struct switch_xml_binding {
	switch_xml_search_function_t function;
	switch_xml_section_t sections;
	void *user_data;
	struct switch_xml_binding *next;
};


static switch_xml_binding_t *BINDINGS = NULL;
static switch_xml_t MAIN_XML_ROOT = NULL;
static switch_memory_pool_t *XML_MEMORY_POOL = NULL;
static switch_thread_rwlock_t *RWLOCK = NULL;
static switch_thread_rwlock_t *B_RWLOCK = NULL;
static switch_mutex_t *XML_LOCK = NULL;


struct xml_section_t {
	const char *name;
	//switch_xml_section_t section;
	uint32_t section;
};

static struct xml_section_t SECTIONS[] = {
	{"result", SWITCH_XML_SECTION_RESULT},
	{"config", SWITCH_XML_SECTION_CONFIG},
	{"directory", SWITCH_XML_SECTION_DIRECTORY},
	{"dialplan", SWITCH_XML_SECTION_DIALPLAN},
	{"phrases", SWITCH_XML_SECTION_PHRASES},
	{NULL, 0}
};

SWITCH_DECLARE(switch_xml_section_t) switch_xml_parse_section_string(const char *str)
{
	size_t x;
	char buf[1024] = "";
	//switch_xml_section_t sections = SWITCH_XML_SECTION_RESULT;
	uint32_t sections = SWITCH_XML_SECTION_RESULT;
	
	if (str) {
		for (x = 0; x < strlen(str); x++) {
			buf[x] = (char) tolower((int) str[x]);
		}
		for (x = 0;; x++) {
			if (!SECTIONS[x].name) {
				break;
			}
			if (strstr(buf, SECTIONS[x].name)) {
				sections |= SECTIONS[x].section;
			}
		}
	}
	return (switch_xml_section_t) sections;
}

SWITCH_DECLARE(switch_status_t) switch_xml_unbind_search_function(switch_xml_binding_t **binding)
{
	switch_xml_binding_t *ptr, *last = NULL;
	switch_status_t status = SWITCH_STATUS_FALSE;


	switch_thread_rwlock_wrlock(B_RWLOCK);
	for (ptr = BINDINGS; ptr; ptr = ptr->next) {
		if (ptr == *binding) {
			if (last) {
				last->next = (*binding)->next;
			} else {
				BINDINGS = (*binding)->next;
			}
			status = SWITCH_STATUS_SUCCESS;
			break;
		}
		last = ptr;
	}
	switch_thread_rwlock_unlock(B_RWLOCK);

	return status;
}

SWITCH_DECLARE(switch_status_t) switch_xml_unbind_search_function_ptr(switch_xml_search_function_t function)
{
	switch_xml_binding_t *ptr, *last = NULL;
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_thread_rwlock_wrlock(B_RWLOCK);
	for (ptr = BINDINGS; ptr; ptr = ptr->next) {
		if (ptr->function == function) {
			if (last) {
				last->next = ptr->next;
			} else {
				BINDINGS = ptr->next;
			}
			status = SWITCH_STATUS_SUCCESS;
		}
		last = ptr;
	}
	switch_thread_rwlock_unlock(B_RWLOCK);

	return status;
}

SWITCH_DECLARE(void) switch_xml_set_binding_sections(switch_xml_binding_t *binding, switch_xml_section_t sections)
{
	switch_assert(binding);
	binding->sections = sections;
}

SWITCH_DECLARE(void) switch_xml_set_binding_user_data(switch_xml_binding_t *binding, void *user_data)
{
	switch_assert(binding);
	binding->user_data = user_data;
}

SWITCH_DECLARE(switch_xml_section_t) switch_xml_get_binding_sections(switch_xml_binding_t *binding)
{
	return binding->sections;
}

SWITCH_DECLARE(void *) switch_xml_get_binding_user_data(switch_xml_binding_t *binding)
{
	return binding->user_data;
}


SWITCH_DECLARE(switch_status_t) switch_xml_bind_search_function_ret(switch_xml_search_function_t function, switch_xml_section_t sections, void *user_data, switch_xml_binding_t **ret_binding)
{
	switch_xml_binding_t *binding = NULL, *ptr = NULL;
	assert(function != NULL);

	if (!(binding = (switch_xml_binding_t *) switch_core_alloc(XML_MEMORY_POOL, sizeof(*binding)))) {
		return SWITCH_STATUS_MEMERR;
	}

	binding->function = function;
	binding->sections = sections;
	binding->user_data = user_data;

	switch_thread_rwlock_wrlock(B_RWLOCK);
	for (ptr = BINDINGS; ptr && ptr->next; ptr = ptr->next);

	if (ptr) {
		ptr->next = binding;
	} else {
		BINDINGS = binding;
	}

	if (ret_binding) {
		*ret_binding = binding;
	}

	switch_thread_rwlock_unlock(B_RWLOCK);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_xml_t) switch_xml_find_child(switch_xml_t node, const char *childname, const char *attrname, const char *value)
{
	switch_xml_t p = NULL;

	if (!(childname && attrname && value)) {
		return node;
	}

	for (p = switch_xml_child(node, childname); p; p = p->next) {
		const char *aname = switch_xml_attr(p, attrname);
		if (aname && value && !strcasecmp(aname, value)) {
			break;
		}
	}

	return p;
}

// returns the first child tag with the given name or NULL if not found
SWITCH_DECLARE(switch_xml_t) switch_xml_child(switch_xml_t xml, const char *name)
{
	xml = (xml) ? xml->child : NULL;
	while (xml && strcmp(name, xml->name))
		xml = xml->sibling;
	return xml;
}

// returns the Nth tag with the same name in the same subsection or NULL if not
// found
switch_xml_t switch_xml_idx(switch_xml_t xml, int idx)
{
	for (; xml && idx; idx--)
		xml = xml->next;
	return xml;
}

// returns the value of the requested tag attribute or "" if not found
SWITCH_DECLARE(const char *) switch_xml_attr_soft(switch_xml_t xml, const char *attr)
{
	const char *ret = switch_xml_attr(xml, attr);

	return ret ? ret : "";
}

// returns the value of the requested tag attribute or NULL if not found
SWITCH_DECLARE(const char *) switch_xml_attr(switch_xml_t xml, const char *attr)
{
	int i = 0, j = 1;
	switch_xml_root_t root = (switch_xml_root_t) xml;

	if (!xml || !xml->attr)
		return NULL;
	while (xml->attr[i] && strcmp(attr, xml->attr[i]))
		i += 2;
	if (xml->attr[i])
		return xml->attr[i + 1];	// found attribute

	while (root->xml.parent)
		root = (switch_xml_root_t) root->xml.parent;	// root tag
	for (i = 0; root->attr[i] && strcmp(xml->name, root->attr[i][0]); i++);
	if (!root->attr[i])
		return NULL;			// no matching default attributes
	while (root->attr[i][j] && strcmp(attr, root->attr[i][j]))
		j += 3;
	return (root->attr[i][j]) ? root->attr[i][j + 1] : NULL;	// found default
}

// same as switch_xml_get but takes an already initialized va_list
static switch_xml_t switch_xml_vget(switch_xml_t xml, va_list ap)
{
	char *name = va_arg(ap, char *);
	int idx = -1;

	if (name && *name) {
		idx = va_arg(ap, int);
		xml = switch_xml_child(xml, name);
	}
	return (idx < 0) ? xml : switch_xml_vget(switch_xml_idx(xml, idx), ap);
}

// Traverses the xml tree to retrieve a specific subtag. Takes a variable
// length list of tag names and indexes. The argument list must be terminated
// by either an index of -1 or an empty string tag name. Example: 
// title = switch_xml_get(library, "shelf", 0, "book", 2, "title", -1);
// This retrieves the title of the 3rd book on the 1st shelf of library.
// Returns NULL if not found.
SWITCH_DECLARE(switch_xml_t) switch_xml_get(switch_xml_t xml,...)
{
	va_list ap;
	switch_xml_t r;

	va_start(ap, xml);
	r = switch_xml_vget(xml, ap);
	va_end(ap);
	return r;
}

// returns a null terminated array of processing instructions for the given
// target
SWITCH_DECLARE(const char **) switch_xml_pi(switch_xml_t xml, const char *target)
{
	switch_xml_root_t root = (switch_xml_root_t) xml;
	int i = 0;

	if (!root)
		return (const char **) SWITCH_XML_NIL;
	while (root->xml.parent)
		root = (switch_xml_root_t) root->xml.parent;	// root tag
	while (root->pi[i] && strcmp(target, root->pi[i][0]))
		i++;					// find target
	return (const char **) ((root->pi[i]) ? root->pi[i] + 1 : SWITCH_XML_NIL);
}

// set an error string and return root
static switch_xml_t switch_xml_err(switch_xml_root_t root, char *s, const char *err, ...)
{
	va_list ap;
	int line = 1;
	char *t, fmt[SWITCH_XML_ERRL];

	for (t = root->s; t && t < s; t++)
		if (*t == '\n')
			line++;
	switch_snprintf(fmt, SWITCH_XML_ERRL, "[error near line %d]: %s", line, err);

	va_start(ap, err);
	vsnprintf(root->err, SWITCH_XML_ERRL, fmt, ap);
	va_end(ap);

	return &root->xml;
}

// Recursively decodes entity and character references and normalizes new lines
// ent is a null terminated array of alternating entity names and values. set t
// to '&' for general entity decoding, '%' for parameter entity decoding, 'c'
// for cdata sections, ' ' for attribute normalization, or '*' for non-cdata
// attribute normalization. Returns s, or if the decoded string is longer than
// s, returns a malloced string that must be freed.
static char *switch_xml_decode(char *s, char **ent, char t)
{
	char *e, *r = s, *m = s;
	long b, c, d, l;

	for (; *s; s++) {			// normalize line endings
		while (*s == '\r') {
			*(s++) = '\n';
			if (*s == '\n')
				memmove(s, (s + 1), strlen(s));
		}
	}

	for (s = r;;) {
		while (*s && *s != '&' && (*s != '%' || t != '%') && !isspace((int) (*s)))
			s++;

		if (!*s)
			break;
		else if (t != 'c' && !strncmp(s, "&#", 2)) {	// character reference
			if (s[2] == 'x')
				c = strtol(s + 3, &e, 16);	// base 16
			else
				c = strtol(s + 2, &e, 10);	// base 10
			if (!c || *e != ';') {
				s++;
				continue;
			}					// not a character ref

			if (c < 0x80)
				*(s++) = (char) c;	// US-ASCII subset
			else {				// multi-byte UTF-8 sequence
				for (b = 0, d = c; d; d /= 2)
					b++;		// number of bits in c
				b = (b - 2) / 5;	// number of bytes in payload
				*(s++) = (char) ((0xFF << (7 - b)) | (c >> (6 * b)));	// head
				while (b)
					*(s++) = (char) (0x80 | ((c >> (6 * --b)) & 0x3F));	// payload
			}

			memmove(s, strchr(s, ';') + 1, strlen(strchr(s, ';')));
		} else if ((*s == '&' && (t == '&' || t == ' ' || t == '*')) || (*s == '%' && t == '%')) {	// entity reference
			for (b = 0; ent[b] && strncmp(s + 1, ent[b], strlen(ent[b])); b += 2);	// find entity in entity list

			if (ent[b++]) {		// found a match
				if ((c = (long) strlen(ent[b])) - 1 > (e = strchr(s, ';')) - s) {
					l = (d = (long) (s - r)) + c + (long) strlen(e);	// new length
					if (l) {
						if (r == m) {
							char *tmp = (char *)malloc(l);
							if (tmp) {
								r = strcpy(tmp, r);
							} else {
								if (r) free(r);
								return NULL;
							}
						} else {
							char *tmp = (char *)realloc(r, l);
							if (tmp) {
								r = tmp;
							} else {
								if (r) free(r);
								return NULL;
							}
						}
					}
					e = strchr((s = r + d), ';');	// fix up pointers
				}

				memmove(s + c, e + 1, strlen(e));	// shift rest of string
				strncpy(s, ent[b], c);	// copy in replacement text
			} else
				s++;			// not a known entity
		} else if ((t == ' ' || t == '*') && isspace((int) (*s)))
			*(s++) = ' ';
		else
			s++;				// no decoding needed
	}

	if (t == '*') {				// normalize spaces for non-cdata attributes
		for (s = r; *s; s++) {
			if ((l = (long) strspn(s, " ")))
				memmove(s, s + l, strlen(s + l) + 1);
			while (*s && *s != ' ')
				s++;
		}
		if (--s >= r && *s == ' ')
			*s = '\0';			// trim any trailing space
	}
	return r;
}

// called when parser finds start of new tag
static void switch_xml_open_tag(switch_xml_root_t root, char *name, char **attr)
{
	switch_xml_t xml = root->cur;

	if (xml->name)
		xml = switch_xml_add_child(xml, name, strlen(xml->txt));
	else
		xml->name = name;		// first open tag

	xml->attr = attr;
	root->cur = xml;			// update tag insertion point
}

// called when parser finds character content between open and closing tag
static void switch_xml_char_content(switch_xml_root_t root, char *s, switch_size_t len, char t)
{
	switch_xml_t xml = root->cur;
	char *m = s;
	switch_size_t l;

	if (!xml || !xml->name || !len)
		return;					// sanity check

	s[len] = '\0';				// null terminate text (calling functions anticipate this)
	len = strlen(s = switch_xml_decode(s, root->ent, t)) + 1;

	if (!*(xml->txt))
		xml->txt = s;			// initial character content
	else {						// allocate our own memory and make a copy
		if ((xml->flags & SWITCH_XML_TXTM)) { // allocate some space
			char *tmp = (char *)realloc(xml->txt, (l = strlen(xml->txt)) + len);
			if (tmp) {
				xml->txt = tmp;
			} else {
				return;
			}
		} else {
			char *tmp = (char *)malloc((l = strlen(xml->txt)) + len);
			if (tmp) {
				xml->txt = strcpy(tmp, xml->txt);
			} else {
				return;
			}
		}
		strcpy(xml->txt + l, s);	// add new char content
		if (s != m)
			free(s);			// free s if it was malloced by switch_xml_decode()
	}

	if (xml->txt != m)
		switch_xml_set_flag(xml, SWITCH_XML_TXTM);
}

// called when parser finds closing tag
static switch_xml_t switch_xml_close_tag(switch_xml_root_t root, char *name, char *s)
{
	if (!root->cur || !root->cur->name || strcmp(name, root->cur->name))
		return switch_xml_err(root, s, "unexpected closing tag </%s>", name);

	root->cur = root->cur->parent;
	return NULL;
}

// checks for circular entity references, returns non-zero if no circular
// references are found, zero otherwise
static int switch_xml_ent_ok(char *name, char *s, char **ent)
{
	int i;

	for (;; s++) {
		while (*s && *s != '&')
			s++;				// find next entity reference
		if (!*s)
			return 1;
		if (!strncmp(s + 1, name, strlen(name)))
			return 0;			// circular ref.
		for (i = 0; ent[i] && strncmp(ent[i], s + 1, strlen(ent[i])); i += 2);
		if (ent[i] && !switch_xml_ent_ok(name, ent[i + 1], ent))
			return 0;
	}
}

// called when the parser finds a processing instruction
static void switch_xml_proc_inst(switch_xml_root_t root, char *s, switch_size_t len)
{
	int i = 0, j = 1;
	char *target = s;
	char **sstmp;
	char *stmp;

	s[len] = '\0';				// null terminate instruction
	if (*(s += strcspn(s, SWITCH_XML_WS))) {
		*s = '\0';				// null terminate target
		s += strspn(s + 1, SWITCH_XML_WS) + 1;	// skip whitespace after target
	}

	if (!root) return;

	if (!strcmp(target, "xml")) {	// <?xml ... ?>
		if ((s = strstr(s, "standalone")) && !strncmp(s + strspn(s + 10, SWITCH_XML_WS "='\"") + 10, "yes", 3))
			root->standalone = 1;
		return;
	}

	if (!root->pi[0]) {
		root->pi = (char ***)malloc(sizeof(char **));
		if (!root->pi) return;
		*(root->pi) = NULL;	//first pi
	}

	while (root->pi[i] && strcmp(target, root->pi[i][0]))
		i++;					// find target
	if (!root->pi[i]) {			// new target
		char ***ssstmp = (char ***)realloc(root->pi, sizeof(char **) * (i + 2));
		if (!ssstmp) return;
		root->pi = ssstmp; 
		if (!root->pi) return;
		root->pi[i] = (char **)malloc(sizeof(char *) * 3);
		if (!root->pi[i]) return;
		root->pi[i][0] = target;
		root->pi[i][1] = (char *) (root->pi[i + 1] = NULL);	// terminate pi list
		root->pi[i][2] = strdup("");	// empty document position list
	}

	while (root->pi[i][j])
		j++;					// find end of instruction list for this target
	sstmp = (char **)realloc(root->pi[i], sizeof(char *) * (j + 3));
	if (!sstmp) return;
	root->pi[i] = sstmp;
	stmp = (char *)realloc(root->pi[i][j + 1], j + 1);
	if (!stmp) return;
	root->pi[i][j + 2] = stmp; 
	strcpy(root->pi[i][j + 2] + j - 1, (root->xml.name) ? ">" : "<");
	root->pi[i][j + 1] = NULL;	// null terminate pi list for this target
	root->pi[i][j] = s;			// set instruction
}

// called when the parser finds an internal doctype subset
static short switch_xml_internal_dtd(switch_xml_root_t root, char *s, switch_size_t len)
{
	char q, *c, *t, *n = NULL, *v, **ent, **pe;
	int i, j;
	char **sstmp;

	pe = (char **)memcpy(malloc(sizeof(SWITCH_XML_NIL)), SWITCH_XML_NIL, sizeof(SWITCH_XML_NIL));
	
	for (s[len] = '\0'; s;) {
		while (*s && *s != '<' && *s != '%')
			s++;				// find next declaration

		if (!*s)
			break;
		else if (!strncmp(s, "<!ENTITY", 8)) {	// parse entity definitions
			c = s += strspn(s + 8, SWITCH_XML_WS) + 8;	// skip white space separator
			n = s + strspn(s, SWITCH_XML_WS "%");	// find name
			*(s = n + strcspn(n, SWITCH_XML_WS)) = ';';	// append ; to name

			v = s + strspn(s + 1, SWITCH_XML_WS) + 1;	// find value
			if ((q = *(v++)) != '"' && q != '\'') {	// skip externals
				s = strchr(s, '>');
				continue;
			}

			for (i = 0, ent = (*c == '%') ? pe : root->ent; ent[i]; i++);
			sstmp = (char **)realloc(ent, (i + 3) * sizeof(char *));	// space for next ent
			if (!sstmp) {
				switch_xml_err(root, v, "Allocation Error!");
				break;
			}
			ent = sstmp;
			if (*c == '%')
				pe = ent;
			else
				root->ent = ent;

			*(++s) = '\0';		// null terminate name
			if ((s = strchr(v, q)))
				*(s++) = '\0';	// null terminate value
			ent[i + 1] = switch_xml_decode(v, pe, '%');	// set value
			ent[i + 2] = NULL;	// null terminate entity list
			if (!switch_xml_ent_ok(n, ent[i + 1], ent)) {	// circular reference
				if (ent[i + 1] != v)
					free(ent[i + 1]);
				switch_xml_err(root, v, "circular entity declaration &%s", n);
				break;
			} else
				ent[i] = n;		// set entity name
		} else if (!strncmp(s, "<!ATTLIST", 9)) {	// parse default attributes
			t = s + strspn(s + 9, SWITCH_XML_WS) + 9;	// skip whitespace separator
			if (!*t) {
				switch_xml_err(root, t, "unclosed <!ATTLIST");
				break;
			}
			if (*(s = t + strcspn(t, SWITCH_XML_WS ">")) == '>')
				continue;
			else
				*s = '\0';		// null terminate tag name
			for (i = 0; root->attr[i] && strcmp(n, root->attr[i][0]); i++);

			while (*(n = ++s + strspn(s, SWITCH_XML_WS)) && *n != '>') {
				if (*(s = n + strcspn(n, SWITCH_XML_WS)))
					*s = '\0';	// attr name
				else {
					switch_xml_err(root, t, "malformed <!ATTLIST");
					break;
				}

				s += strspn(s + 1, SWITCH_XML_WS) + 1;	// find next token
				c = (strncmp(s, "CDATA", 5)) ? (char *)"*" : (char *)" ";	// is it cdata?
				if (!strncmp(s, "NOTATION", 8))
					s += strspn(s + 8, SWITCH_XML_WS) + 8;
				s = (*s == '(') ? strchr(s, ')') : s + strcspn(s, SWITCH_XML_WS);
				if (!s) {
					switch_xml_err(root, t, "malformed <!ATTLIST");
					break;
				}

				s += strspn(s, SWITCH_XML_WS ")");	// skip white space separator
				if (!strncmp(s, "#FIXED", 6))
					s += strspn(s + 6, SWITCH_XML_WS) + 6;
				if (*s == '#') {	// no default value
					s += strcspn(s, SWITCH_XML_WS ">") - 1;
					if (*c == ' ')
						continue;	// cdata is default, nothing to do
					v = NULL;
				} else if ((*s == '"' || *s == '\'') &&	// default value
						   (s = strchr(v = s + 1, *s)))
					*s = '\0';
				else {
					switch_xml_err(root, t, "malformed <!ATTLIST");
					break;
				}

				if (!root->attr[i]) {	// new tag name
					root->attr = (!i) ? (char ***)malloc(2 * sizeof(char **))
						: (char ***)realloc(root->attr, (i + 2) * sizeof(char **));
					root->attr[i] = (char **)malloc(2 * sizeof(char *));
					root->attr[i][0] = t;	// set tag name
					root->attr[i][1] = (char *) (root->attr[i + 1] = NULL);
				}

				for (j = 1; root->attr[i][j]; j += 3);	// find end of list
				sstmp = (char **)realloc(root->attr[i], (j + 4) * sizeof(char *));

				if (!sstmp) {
					switch_xml_err(root, t, "Allocation Error!");
					break;
				}

				root->attr[i] = sstmp;
				root->attr[i][j + 3] = NULL;	// null terminate list
				root->attr[i][j + 2] = c;	// is it cdata?
				root->attr[i][j + 1] = (v) ? switch_xml_decode(v, root->ent, *c)
					: NULL;
				root->attr[i][j] = n;	// attribute name 
			}
		} else if (!strncmp(s, "<!--", 4))
			s = strstr(s + 4, "-->");	// comments
		else if (!strncmp(s, "<?", 2)) {	// processing instructions
			if ((s = strstr(c = s + 2, "?>")))
				switch_xml_proc_inst(root, c, s++ - c);
		} else if (*s == '<')
			s = strchr(s, '>');	// skip other declarations
		else if (*(s++) == '%' && !root->standalone)
			break;
	}

	free(pe);
	return !*root->err;
}

// Converts a UTF-16 string to UTF-8. Returns a new string that must be freed
// or NULL if no conversion was needed.
static char *switch_xml_str2utf8(char **s, switch_size_t *len)
{
	char *u;
	switch_size_t l = 0, sl, max = *len;
	long c, d;
	int b, be = (**s == '\xFE') ? 1 : (**s == '\xFF') ? 0 : -1;

	if (be == -1)
		return NULL;			// not UTF-16

	u = (char *)malloc(max);
	for (sl = 2; sl < *len - 1; sl += 2) {
		c = (be) ? (((*s)[sl] & 0xFF) << 8) | ((*s)[sl + 1] & 0xFF)	//UTF-16BE
			: (((*s)[sl + 1] & 0xFF) << 8) | ((*s)[sl] & 0xFF);	//UTF-16LE
		if (c >= 0xD800 && c <= 0xDFFF && (sl += 2) < *len - 1) {	// high-half
			d = (be) ? (((*s)[sl] & 0xFF) << 8) | ((*s)[sl + 1] & 0xFF)
				: (((*s)[sl + 1] & 0xFF) << 8) | ((*s)[sl] & 0xFF);
			c = (((c & 0x3FF) << 10) | (d & 0x3FF)) + 0x10000;
		}

		while (l + 6 > max) {
			char *tmp;
			tmp = (char *)realloc(u, max += SWITCH_XML_BUFSIZE);
			if (!tmp) return NULL;
			u = tmp;
		}
		if (c < 0x80)
			u[l++] = (char) c;	// US-ASCII subset
		else {					// multi-byte UTF-8 sequence
			for (b = 0, d = c; d; d /= 2)
				b++;			// bits in c
			b = (b - 2) / 5;	// bytes in payload
			u[l++] = (char) ((0xFF << (7 - b)) | (c >> (6 * b)));	// head
			while (b)
				u[l++] = (char) (0x80 | ((c >> (6 * --b)) & 0x3F));	// payload
		}
	}
	return *s = (char *)realloc(u, *len = l);
}

// frees a tag attribute list
static void switch_xml_free_attr(char **attr)
{
	int i = 0;
	char *m;

	if (!attr || attr == SWITCH_XML_NIL)
		return;					// nothing to free
	while (attr[i])
		i += 2;					// find end of attribute list
	m = attr[i + 1];			// list of which names and values are malloced
	for (i = 0; m[i]; i++) {
		if (m[i] & SWITCH_XML_NAMEM)
			free(attr[i * 2]);
		if (m[i] & SWITCH_XML_TXTM)
			free(attr[(i * 2) + 1]);
	}
	free(m);
	free(attr);
}

// parse the given xml string and return an switch_xml structure
SWITCH_DECLARE(switch_xml_t) switch_xml_parse_str(char *s, switch_size_t len)
{
	switch_xml_root_t root = (switch_xml_root_t) switch_xml_new(NULL);
	char q, e, *d, **attr, **a = NULL;	// initialize a to avoid compile warning
	int l, i, j;

	root->m = s;
	if (!len)
		return switch_xml_err(root, s, "root tag missing");
	root->u = switch_xml_str2utf8(&s, &len);	// convert utf-16 to utf-8
	root->e = (root->s = s) + len;	// record start and end of work area

	e = s[len - 1];				// save end char
	s[len - 1] = '\0';			// turn end char into null terminator

	while (*s && *s != '<')
		s++;					// find first tag
	if (!*s)
		return switch_xml_err(root, s, "root tag missing");

	for (;;) {
		attr = (char **) SWITCH_XML_NIL;
		d = ++s;

		if (isalpha((int) (*s)) || *s == '_' || *s == ':' || (int8_t) *s < '\0') {	// new tag
			if (!root->cur)
				return switch_xml_err(root, d, "markup outside of root element");

			s += strcspn(s, SWITCH_XML_WS "/>");
			while (isspace((int) (*s)))
				*(s++) = '\0';	// null terminate tag name

			if (*s && *s != '/' && *s != '>')	// find tag in default attr list
				for (i = 0; (a = root->attr[i]) && strcmp(a[0], d); i++);

			for (l = 0; *s && *s != '/' && *s != '>'; l += 2) {	// new attrib
				attr = (l) ? (char **)realloc(attr, (l + 4) * sizeof(char *))
					: (char **)malloc(4 * sizeof(char *));	// allocate space
				attr[l + 3] = (l) ? (char *)realloc(attr[l + 1], (l / 2) + 2)
					: (char *)malloc(2);	// mem for list of maloced vals
				strcpy(attr[l + 3] + (l / 2), " ");	// value is not malloced
				attr[l + 2] = NULL;	// null terminate list
				attr[l + 1] = (char *)"";	// temporary attribute value
				attr[l] = s;	// set attribute name

				s += strcspn(s, SWITCH_XML_WS "=/>");
				if (*s == '=' || isspace((int) (*s))) {
					*(s++) = '\0';	// null terminate tag attribute name
					q = *(s += strspn(s, SWITCH_XML_WS "="));
					if (q == '"' || q == '\'') {	// attribute value
						attr[l + 1] = ++s;
						while (*s && *s != q)
							s++;
						if (*s)
							*(s++) = '\0';	// null terminate attribute val
						else {
							switch_xml_free_attr(attr);
							return switch_xml_err(root, d, "missing %c", q);
						}

						for (j = 1; a && a[j] && strcmp(a[j], attr[l]); j += 3);
						attr[l + 1] = switch_xml_decode(attr[l + 1], root->ent, (a && a[j]) ? *a[j + 2] : ' ');
						if (attr[l + 1] < d || attr[l + 1] > s)
							attr[l + 3][l / 2] = SWITCH_XML_TXTM;	// value malloced
					}
				}
				while (isspace((int) (*s)))
					s++;
			}

			if (*s == '/') {	// self closing tag
				*(s++) = '\0';
				if ((*s && *s != '>') || (!*s && e != '>')) {
					if (l)
						switch_xml_free_attr(attr);
					return switch_xml_err(root, d, "missing >");
				}
				switch_xml_open_tag(root, d, attr);
				switch_xml_close_tag(root, d, s);
			} else if ((q = *s) == '>' || (!*s && e == '>')) {	// open tag
				*s = '\0';		// temporarily null terminate tag name
				switch_xml_open_tag(root, d, attr);
				*s = q;
			} else {
				if (l)
					switch_xml_free_attr(attr);
				return switch_xml_err(root, d, "missing >");
			}
		} else if (*s == '/') {	// close tag
			s += strcspn(d = s + 1, SWITCH_XML_WS ">") + 1;
			if (!(q = *s) && e != '>')
				return switch_xml_err(root, d, "missing >");
			*s = '\0';			// temporarily null terminate tag name
			if (switch_xml_close_tag(root, d, s))
				return &root->xml;
			if (isspace((int) (*s = q)))
				s += strspn(s, SWITCH_XML_WS);
		} else if (!strncmp(s, "!--", 3)) {	// xml comment
			if (!(s = strstr(s + 3, "--")) || (*(s += 2) != '>' && *s) || (!*s && e != '>'))
				return switch_xml_err(root, d, "unclosed <!--");
		} else if (!strncmp(s, "![CDATA[", 8)) {	// cdata
			if ((s = strstr(s, "]]>")))
				switch_xml_char_content(root, d + 8, (s += 2) - d - 10, 'c');
			else
				return switch_xml_err(root, d, "unclosed <![CDATA[");
		} else if (!strncmp(s, "!DOCTYPE", 8)) {	// dtd
			for (l = 0; *s && ((!l && *s != '>') || (l && (*s != ']' || *(s + strspn(s + 1, SWITCH_XML_WS) + 1) != '>'))); l = (*s == '[') ? 1 : l)
				s += strcspn(s + 1, "[]>") + 1;
			if (!*s && e != '>')
				return switch_xml_err(root, d, "unclosed <!DOCTYPE");
			d = (l) ? strchr(d, '[') + 1 : d;
			if (l && !switch_xml_internal_dtd(root, d, s++ - d))
				return &root->xml;
		} else if (*s == '?') {	// <?...?> processing instructions
			do {
				s = strchr(s, '?');
			} while (s && *(++s) && *s != '>');
			if (!s || (!*s && e != '>'))
				return switch_xml_err(root, d, "unclosed <?");
			else
				switch_xml_proc_inst(root, d + 1, s - d - 2);
		} else
			return switch_xml_err(root, d, "unexpected <");

		if (!s || !*s)
			break;
		*s = '\0';
		d = ++s;
		if (*s && *s != '<') {	// tag character content
			while (*s && *s != '<')
				s++;
			if (*s)
				switch_xml_char_content(root, d, s - d, '&');
			else
				break;
		} else if (!*s)
			break;
	}

	if (!root->cur)
		return &root->xml;
	else if (!root->cur->name)
		return switch_xml_err(root, d, "root tag missing");
	else
		return switch_xml_err(root, d, "unclosed tag <%s>", root->cur->name);
}

// Wrapper for switch_xml_parse_str() that accepts a file stream. Reads the entire
// stream into memory and then parses it. For xml files, use switch_xml_parse_file()
// or switch_xml_parse_fd()
SWITCH_DECLARE(switch_xml_t) switch_xml_parse_fp(FILE * fp)
{
	switch_xml_root_t root;
	switch_size_t l, len = 0;
	char *s;

	if (!(s = (char *)malloc(SWITCH_XML_BUFSIZE)))
		return NULL;
	do {
		len += (l = fread((s + len), 1, SWITCH_XML_BUFSIZE, fp));
		if (l == SWITCH_XML_BUFSIZE) {
			char *tmp = (char *)realloc(s, len + SWITCH_XML_BUFSIZE);
			if (!tmp) {
				free(s);
				return NULL;
			}
			s = tmp;
		}
	} while (s && l == SWITCH_XML_BUFSIZE);

	if (!s)
		return NULL;
	root = (switch_xml_root_t) switch_xml_parse_str(s, len);
	root->dynamic = 1;			// so we know to free s in switch_xml_free()
	return &root->xml;
}

// A wrapper for switch_xml_parse_str() that accepts a file descriptor. First
// attempts to mem map the file. Failing that, reads the file into memory.
// Returns NULL on failure.
SWITCH_DECLARE(switch_xml_t) switch_xml_parse_fd(int fd)
{
	switch_xml_root_t root;
	struct stat st;
	switch_size_t l;
	void *m;

	if (fd < 0)
		return NULL;
	fstat(fd, &st);

#ifdef HAVE_MMAP
	l = (st.st_size + sysconf(_SC_PAGESIZE) - 1) & ~(sysconf(_SC_PAGESIZE) - 1);
	if ((m = mmap(NULL, l, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)) != MAP_FAILED) {
		madvise(m, l, MADV_SEQUENTIAL);	// optimize for sequential access
		root = (switch_xml_root_t) switch_xml_parse_str(m, st.st_size);
		madvise(m, root->len = l, MADV_NORMAL);	// put it back to normal
	} else {					// mmap failed, read file into memory
#endif // HAVE_MMAP
		m = malloc(st.st_size);
		if (!m) return NULL;
		l = read(fd, m, st.st_size);
		root = (switch_xml_root_t) switch_xml_parse_str((char *)m, l);
		root->dynamic = 1;		// so we know to free s in switch_xml_free()
#ifdef HAVE_MMAP
	}
#endif // HAVE_MMAP
	return &root->xml;
}

static char *expand_vars(char *buf, char *ebuf, switch_size_t elen, switch_size_t *newlen)
{
	char *var, *val;
	char *rp = buf;
	char *wp = ebuf;
	char *ep = ebuf + elen - 1;

	if (!(var = strstr(rp, "$${"))) {
		*newlen = strlen(buf);
		return buf;
	}

	while (*rp && wp < ep) {

		if (*rp == '$' && *(rp + 1) == '$' && *(rp + 2) == '{') {
			char *e = strchr(rp, '}');

			if (e) {
				rp += 3;
				var = rp;
				*e++ = '\0';
				rp = e;
				if ((val = switch_core_get_variable(var))) {
					char *p;
					for (p = val; p && *p && wp <= ep; p++) {
						*wp++ = *p;
					}
				}
			}
		}

		*wp++ = *rp++;
	}
	*wp++ = '\0';
	*newlen = strlen(ebuf);

	return ebuf;
}

static int preprocess_exec(const char *cwd, const char *command, int write_fd, int rlevel)
{
#ifdef WIN32
	char message[] = "<!-- exec not implemented in windows yet -->";

	if (write(write_fd, message, sizeof(message)) < 0) {
		goto end;
	}

#else
	int fds[2], pid = 0;

	if (pipe(fds)) {
		goto end;
	} else { /* good to go*/
		pid = fork();

		if (pid < 0) { /* ok maybe not */
			close(fds[0]);
			close(fds[1]);
			goto end;
		} else if (pid) { /* parent */
			char buf[1024] = "";
			int bytes;
			close(fds[1]);
			while ((bytes = read(fds[0], buf, sizeof(buf))) > 0) {
				if (write(write_fd, buf, bytes) <= 0) {
					break;
				}
			}
			close(fds[0]);
		} else { /*  child */
			close(fds[0]);
			dup2(fds[1], STDOUT_FILENO);
			switch_system(command, SWITCH_TRUE);
			close(fds[1]);
			exit(0);
		}
	}
#endif
 end:

	return write_fd;
	
}


static int preprocess_glob(const char *cwd, const char *pattern, int write_fd, int rlevel)
{
	char *full_path = NULL;
	char *dir_path = NULL, *e = NULL;
    glob_t glob_data;
	size_t n;

	if (!switch_is_file_path(pattern)) {
		full_path = switch_mprintf("%s%s%s", cwd, SWITCH_PATH_SEPARATOR, pattern);
		pattern = full_path;
	}
	
    if (glob(pattern, GLOB_NOCHECK, NULL, &glob_data) != 0) {
		if (stderr) {
			fprintf(stderr, "Error including %s\n", pattern);
		}
		goto end;
	}

	for (n = 0; n < glob_data.gl_pathc; ++n) {
		dir_path = strdup(glob_data.gl_pathv[n]);
		switch_assert(dir_path);
		if ((e = strrchr(dir_path, *SWITCH_PATH_SEPARATOR))) {
			*e = '\0';
		}
		if (preprocess(dir_path, glob_data.gl_pathv[n], write_fd, rlevel) < 0) {
			const char *reason = strerror(errno);
			if (rlevel > 100) {
				reason = "Maximum recursion limit reached";
			}
			fprintf(stderr, "Error including %s (%s)\n", pattern, reason);
		}
		free(dir_path);
	}
    globfree(&glob_data);

 end:

	switch_safe_free(full_path);

	return write_fd;
}

static int preprocess(const char *cwd, const char *file, int write_fd, int rlevel)
{
	int read_fd = -1;
	switch_size_t cur = 0, ml = 0;
	char *q, *cmd, buf[2048], ebuf[8192];
	char *tcmd, *targ;

	if ((read_fd = open(file, O_RDONLY, 0)) < 0) {
		return read_fd;
	}

	if (rlevel > 100) {
		return -1;
	}

	while ((cur = switch_fd_read_line(read_fd, buf, sizeof(buf))) > 0) {
		char *arg, *e;
		char *bp = expand_vars(buf, ebuf, sizeof(ebuf), &cur);

		/* we ignore <include> or </include> for the sake of validators as well as <?xml version="1.0"?> type stuff */
		if (strstr(buf, "<include>") || strstr(buf, "</include>") || strstr(buf, "<?")) {
			continue;
		}

		if (ml) {
			if ((e = strstr(buf, "-->"))) {
				ml = 0;
				bp = e + 3;
				cur = strlen(bp);
			} else {
				continue;
			}
		}

		if ((tcmd = (char *)switch_stristr("X-pre-process", bp))) {
			if (*(tcmd-1) != '<') {
				continue;
			}
			if ((e = strstr(tcmd, "/>"))) {
				*e += 2;
				*e = '\0';
				if (write(write_fd, e, (unsigned) strlen(e)) != (int) strlen(e)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Short write!\n");
				}
			}
			
			if (!(tcmd = (char *)switch_stristr("cmd", tcmd))) {
				continue;
			}

			if (!(tcmd = (char *)switch_stristr("=", tcmd))) {
				continue;
			}

			if (!(tcmd = (char *)switch_stristr("\"", tcmd))) {
				continue;
			}
			
			tcmd++;

			
			if ((e = strchr(tcmd, '"'))) {
				*e++ = '\0';
			}

			if (!(targ = (char *)switch_stristr("data", e))) {
				continue;
			}

			if (!(targ = (char *)switch_stristr("=", targ))) {
				continue;
			}

			if (!(targ = (char *)switch_stristr("\"", targ))) {
				continue;
			}

			targ++;

			if ((e = strchr(targ, '"'))) {
				*e++ = '\0';
			}
			
			if (!strcasecmp(tcmd, "set")) {
				char *name = (char *)targ;
				char *val = strchr(name, '=');
				
				if (val) {
					char *ve = val++;
					while (*val && *val == ' ') {
							val++;
					}
					*ve-- = '\0';
					while (*ve && *ve == ' ') {
						*ve-- = '\0';
					}
				}
				
				if (name && val) {
					switch_core_set_variable(name, val);
				}
				
			} else if (!strcasecmp(tcmd, "include")) {
				preprocess_glob(cwd, targ, write_fd, rlevel + 1);
			} else if (!strcasecmp(tcmd, "exec")) {
				preprocess_exec(cwd, targ, write_fd, rlevel + 1);
			}

			continue;
		}
		
		if ((cmd = strstr(bp, "<!--#"))) {
			if (write(write_fd, bp, (unsigned) (cmd - bp)) != (cmd - bp)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Short write!\n");
			}
			if ((e = strstr(cmd, "-->"))) {
				*e = '\0';
				e += 3;
				if (write(write_fd, e, (unsigned) strlen(e)) != (int) strlen(e)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Short write!\n");
				}
			} else {
				ml++;
			}

			cmd += 5;
			if ((e = strchr(cmd, '\r')) || (e = strchr(cmd, '\n'))) {
				*e = '\0';
			}

			if ((arg = strchr(cmd, ' '))) {
				*arg++ = '\0';
				if ((q = strchr(arg, '"'))) {
					char *qq = q + 1;

					if ((qq = strchr(qq, '"'))) {
						*qq = '\0';
						arg = q + 1;
					}
				}

				if (!strcasecmp(cmd, "set")) {
					char *name = arg;
					char *val = strchr(name, '=');

					if (val) {
						char *ve = val++;
						while (*val && *val == ' ') {
							val++;
						}
						*ve-- = '\0';
						while (*ve && *ve == ' ') {
							*ve-- = '\0';
						}
					}

					if (name && val) {
						switch_core_set_variable(name, val);
					}

				} else if (!strcasecmp(cmd, "include")) {
					preprocess_glob(cwd, arg, write_fd, rlevel + 1);
				} else if (!strcasecmp(cmd, "exec")) {
					preprocess_exec(cwd, arg, write_fd, rlevel + 1);
				}
			}

			continue;
		}

		if (write(write_fd, bp, (unsigned) cur) != (int) cur) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Short write!\n");
		}
	}

	close(read_fd);
	return write_fd;
}

SWITCH_DECLARE(switch_xml_t) switch_xml_parse_file_simple(const char *file)
{
	int fd = -1;
	struct stat st;
	switch_size_t l;
	void *m;
	switch_xml_root_t root;

	if ((fd = open(file, O_RDONLY, 0)) > -1) {
		fstat(fd, &st);
		m = malloc(st.st_size);
		switch_assert(m);
		l = read(fd, m, st.st_size);
		root = (switch_xml_root_t) switch_xml_parse_str((char *)m, l);
		root->dynamic = 1;
		close(fd);
		return &root->xml;
	}
	
	return NULL;
}

SWITCH_DECLARE(switch_xml_t) switch_xml_parse_file(const char *file)
{
	int fd = -1, write_fd = -1;
	switch_xml_t xml = NULL;
	char *new_file = NULL;
	const char *abs;

	if ((abs = strrchr(file, '/')) || (abs = strrchr(file, '\\'))) {
		abs++;
	} else {
		abs = file;
	}
	
	if (!(new_file = switch_mprintf("%s%s%s.fsxml", SWITCH_GLOBAL_dirs.log_dir, SWITCH_PATH_SEPARATOR, abs))) {
		return NULL;
	}

	if ((write_fd = open(new_file, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) < 0) {
		goto done;
	}

	if (preprocess(SWITCH_GLOBAL_dirs.conf_dir, file, write_fd, 0) > -1) {
		close(write_fd);
		write_fd = -1;
		if ((fd = open(new_file, O_RDONLY, 0)) > -1) {
			if ((xml = switch_xml_parse_fd(fd))) {
				xml->free_path = new_file;
				new_file = NULL;
			}
			close(fd);
			fd = -1;
		}
	}

  done:
	if (write_fd > -1) {
		close(write_fd);
	}
	if (fd > -1) {
		close(fd);
	}
	switch_safe_free(new_file);
	return xml;
}

SWITCH_DECLARE(switch_status_t) switch_xml_locate(const char *section,
												  const char *tag_name,
												  const char *key_name, const char *key_value, switch_xml_t * root, switch_xml_t * node,
												  switch_event_t *params)
{
	switch_xml_t conf = NULL;
	switch_xml_t tag = NULL;
	switch_xml_t xml = NULL;
	switch_xml_binding_t *binding;
	uint8_t loops = 0;

	switch_thread_rwlock_rdlock(B_RWLOCK);

	for (binding = BINDINGS; binding; binding = binding->next) {
		switch_xml_section_t sections = switch_xml_parse_section_string(section);

		if (binding->sections && !(sections & binding->sections)) {
			continue;
		}

		if ((xml = binding->function(section, tag_name, key_name, key_value, params, binding->user_data))) {
			const char *err = NULL;

			err = switch_xml_error(xml);
			if (switch_strlen_zero(err)) {
				if ((conf = switch_xml_find_child(xml, "section", "name", "result"))) {
					switch_xml_t p;
					const char *aname;

					if ((p = switch_xml_child(conf, "result"))) {
						aname = switch_xml_attr(p, "status");
						if (aname && !strcasecmp(aname, "not found")) {
							switch_xml_free(xml);
							xml = NULL;
							continue;
						}
					}
				}
				break;
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error[%s]\n", err);
				switch_xml_free(xml);
				xml = NULL;
			}
		}
	}
	switch_thread_rwlock_unlock(B_RWLOCK);

	for (;;) {
		if (!xml) {
			if (!(xml = switch_xml_root())) {
				*node = NULL;
				*root = NULL;
				return SWITCH_STATUS_FALSE;
			}
		}

		if ((conf = switch_xml_find_child(xml, "section", "name", section)) && (tag = switch_xml_find_child(conf, tag_name, key_name, key_value))) {
			*node = tag;
			*root = xml;
			return SWITCH_STATUS_SUCCESS;
		} else {
			switch_xml_free(xml);
			xml = NULL;
			*node = NULL;
			*root = NULL;
			if (loops++ > 1) {
				break;
			}
		}
	}

	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_xml_locate_domain(const char *domain_name, switch_event_t *params, switch_xml_t *root, switch_xml_t *domain)
{
	switch_event_t *my_params = NULL;
	switch_status_t status;
	*domain = NULL;

	if (!params) {
		switch_event_create(&my_params, SWITCH_EVENT_REQUEST_PARAMS);
		switch_assert(my_params);
		switch_event_add_header_string(my_params, SWITCH_STACK_BOTTOM, "domain", domain_name);
		params = my_params;
	}

	status = switch_xml_locate("directory", "domain", "name", domain_name, root, domain, params);
	if (my_params) {
		switch_event_destroy(&my_params);
	}
	return status;
}

SWITCH_DECLARE(switch_status_t) switch_xml_locate_user(const char *key,
													   const char *user_name,
													   const char *domain_name, 
													   const char *ip, 
													   switch_xml_t *root,
													   switch_xml_t *domain,
													   switch_xml_t *user,
													   switch_event_t *params)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_event_t *my_params = NULL;

	*root = NULL;
	*user = NULL;
	*domain = NULL;

	if (!params) {
		switch_event_create(&my_params, SWITCH_EVENT_REQUEST_PARAMS);
		switch_assert(my_params);
		params = my_params;
	}

	switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "key", key);

	if (user_name) {
		switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "user", user_name);
	}

	if (domain_name) {
		switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "domain", domain_name);
	}

	if (ip) {
		switch_event_add_header_string(params, SWITCH_STACK_BOTTOM, "ip", ip);
	}
	
	if ((status = switch_xml_locate_domain(domain_name, params, root, domain)) != SWITCH_STATUS_SUCCESS) {
		goto end;
	}
	
	status = SWITCH_STATUS_FALSE;

	if (ip) {
		if ((*user = switch_xml_find_child(*domain, "user", "ip", ip))) {
			status = SWITCH_STATUS_SUCCESS;
			goto end;
		}
	} 

	if (user_name) {
		if (params != my_params && switch_event_get_header(params, (char *) "mailbox")) {
			if ((*user = switch_xml_find_child(*domain, "user", "mailbox", user_name))) {
				status = SWITCH_STATUS_SUCCESS;
				goto end;
			}
		}

		if ((*user = switch_xml_find_child(*domain, "user", key, user_name))) {
			status = SWITCH_STATUS_SUCCESS;
			goto end;
		}
	}

 end:

	if (my_params) {
		switch_event_destroy(&my_params);
	}

	return status;
}

SWITCH_DECLARE(switch_xml_t) switch_xml_root(void)
{
	switch_thread_rwlock_rdlock(RWLOCK);
	return MAIN_XML_ROOT;
}


struct destroy_xml {
	switch_xml_t xml;
	switch_memory_pool_t *pool;
};

static void *SWITCH_THREAD_FUNC destroy_thread(switch_thread_t *thread, void *obj)
{
	struct destroy_xml *dx = (struct destroy_xml *) obj;
	switch_memory_pool_t *pool = dx->pool;
	switch_xml_free(dx->xml);
	switch_core_destroy_memory_pool(&pool);
	return NULL;
}

SWITCH_DECLARE(void) switch_xml_free_in_thread(switch_xml_t xml, int stacksize)
{
	switch_thread_t *thread;
    switch_threadattr_t *thd_attr;
	switch_memory_pool_t *pool = NULL;
	struct destroy_xml *dx;

	switch_core_new_memory_pool(&pool);

	switch_threadattr_create(&thd_attr, pool);
    switch_threadattr_detach_set(thd_attr, 1);
	// TBD figure out how much space we need by looking at the xml_t when stacksize == 0
    switch_threadattr_stacksize_set(thd_attr, stacksize);

	dx = switch_core_alloc(pool, sizeof(*dx));
	dx->pool = pool;
	dx->xml = xml;

	switch_thread_create(&thread, thd_attr, destroy_thread, dx, pool);

}

static char not_so_threadsafe_error_buffer[256] = "";

SWITCH_DECLARE(switch_xml_t) switch_xml_open_root(uint8_t reload, const char **err)
{
	char path_buf[1024];
	uint8_t hasmain = 0, errcnt = 0;
	switch_xml_t new_main;

	switch_mutex_lock(XML_LOCK);

	if (MAIN_XML_ROOT) {
		hasmain++;

		if (!reload) {
			switch_mutex_unlock(XML_LOCK);
			return switch_xml_root();
		}
		switch_thread_rwlock_wrlock(RWLOCK);
	}

	switch_snprintf(path_buf, sizeof(path_buf), "%s%s%s", SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR, "freeswitch.xml");
	if ((new_main = switch_xml_parse_file(path_buf))) {
		*err = switch_xml_error(new_main);
		switch_copy_string(not_so_threadsafe_error_buffer, *err, sizeof(not_so_threadsafe_error_buffer));
		*err = not_so_threadsafe_error_buffer;
		if (!switch_strlen_zero(*err)) {
			switch_xml_free(new_main);
			new_main = NULL;
			errcnt++;
		} else {
			switch_xml_t old_root;
			*err = "Success";
			old_root = MAIN_XML_ROOT;
			MAIN_XML_ROOT = new_main;
			switch_set_flag(MAIN_XML_ROOT, SWITCH_XML_ROOT);
			switch_xml_free(old_root);
			//switch_xml_free_in_thread(old_root);
		}
	} else {
		*err = "Cannot Open log directory or XML Root!";
		errcnt++;
	}

	if (hasmain) {
		switch_thread_rwlock_unlock(RWLOCK);
	}

	switch_mutex_unlock(XML_LOCK);

	if (errcnt == 0) {
		switch_event_t *event;
		if (switch_event_create(&event, SWITCH_EVENT_RELOADXML) == SWITCH_STATUS_SUCCESS) {
			if (switch_event_fire(&event) != SWITCH_STATUS_SUCCESS) {
				switch_event_destroy(&event);
			}
		}
		return switch_xml_root();
	}

	return NULL;
}

SWITCH_DECLARE(switch_status_t) switch_xml_init(switch_memory_pool_t *pool, const char **err)
{
	switch_xml_t xml;
	XML_MEMORY_POOL = pool;
	*err = "Success";

	switch_mutex_init(&XML_LOCK, SWITCH_MUTEX_NESTED, XML_MEMORY_POOL);
	switch_thread_rwlock_create(&RWLOCK, XML_MEMORY_POOL);
	switch_thread_rwlock_create(&B_RWLOCK, XML_MEMORY_POOL);

	assert(pool != NULL);

	if ((xml = switch_xml_open_root(FALSE, err))) {
		switch_xml_free(xml);
		return SWITCH_STATUS_SUCCESS;
	} else {
		return SWITCH_STATUS_FALSE;
	}
}

SWITCH_DECLARE(switch_status_t) switch_xml_destroy(void)
{
	if (MAIN_XML_ROOT) {
		switch_xml_t xml = MAIN_XML_ROOT;
		MAIN_XML_ROOT = NULL;
		switch_xml_free(xml);
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_xml_t) switch_xml_open_cfg(const char *file_path, switch_xml_t *node, switch_event_t *params)
{
	switch_xml_t xml = NULL, cfg = NULL;

	*node = NULL;

	assert(MAIN_XML_ROOT != NULL);

	if (switch_xml_locate("configuration", "configuration", "name", file_path, &xml, &cfg, params) == SWITCH_STATUS_SUCCESS) {
		*node = cfg;
	}

	return xml;

}

// Encodes ampersand sequences appending the results to *dst, reallocating *dst
// if length exceeds max. a is non-zero for attribute encoding. Returns *dst
static char *switch_xml_ampencode(const char *s, switch_size_t len, char **dst, switch_size_t *dlen, switch_size_t *max, short a)
{
	const char *e = NULL;
	int immune = 0;

	if (!(s && *s)) return *dst;	

	if (len) {
		e = s + len;
	}

	while (s != e) {
		while (*dlen + 10 > *max) {
			char *tmp = (char *)realloc(*dst, *max += SWITCH_XML_BUFSIZE);
			if (!tmp) return *dst;
			*dst = tmp;
		}

		if (immune) {
			if (*s == '\0') {
				return *dst;
			}
			(*dst)[(*dlen)++] = *s;
		} else 
			switch (*s) {
		case '\0':
			return *dst;
		case '&':
			*dlen += sprintf(*dst + *dlen, "&amp;");
			break;
		case '<':
			if (*(s+1) == '!') {
				(*dst)[(*dlen)++] = *s;
				immune++;
				break;
			}
			*dlen += sprintf(*dst + *dlen, "&lt;");
			break;
		case '>':
			*dlen += sprintf(*dst + *dlen, "&gt;");
			break;
		case '"':
			*dlen += sprintf(*dst + *dlen, (a) ? "&quot;" : "\"");
			break;
		case '\n':
			*dlen += sprintf(*dst + *dlen, (a) ? "&#xA;" : "\n");
			break;
		case '\t':
			*dlen += sprintf(*dst + *dlen, (a) ? "&#x9;" : "\t");
			break;
		case '\r':
			*dlen += sprintf(*dst + *dlen, "&#xD;");
			break;
		default:
			(*dst)[(*dlen)++] = *s;
		}
		s++;
	}
	return *dst;
}

#define XML_INDENT "  "
// Recursively converts each tag to xml appending it to *s. Reallocates *s if
// its length exceeds max. start is the location of the previous tag in the
// parent tag's character content. Returns *s.
static char *switch_xml_toxml_r(switch_xml_t xml, char **s, switch_size_t *len, switch_size_t *max, switch_size_t start, char ***attr, uint32_t * count)
{
	int i, j;
	char *txt = (char *)(xml->parent) ? xml->parent->txt : (char *)"";
	switch_size_t off = 0;
	uint32_t lcount = 0;

	// parent character content up to this tag
	*s = switch_xml_ampencode(txt + start, xml->off - start, s, len, max, 0);

	while (*len + strlen(xml->name) + 5 + (strlen(XML_INDENT) * (*count)) + 1 > *max) {	// reallocate s
		char *tmp = (char *)realloc(*s, *max += SWITCH_XML_BUFSIZE);
		if (!tmp) return *s;
		*s = tmp;
	}

	if (*(*s + (*len) - 1) == '>') {
		*len += sprintf(*s + *len, "\n");	// indent
	}
	for (lcount = 0; lcount < *count; lcount++) {
		*len += sprintf(*s + *len, "%s", XML_INDENT);	// indent
	}

	*len += sprintf(*s + *len, "<%s", xml->name);	// open tag
	for (i = 0; xml->attr[i]; i += 2) {	// tag attributes
		if (switch_xml_attr(xml, xml->attr[i]) != xml->attr[i + 1])
			continue;
		while (*len + strlen(xml->attr[i]) + 7 + (strlen(XML_INDENT) * (*count)) > *max) {	// reallocate s
			char *tmp = (char *)realloc(*s, *max += SWITCH_XML_BUFSIZE);
			if (!tmp) return *s;
			*s = tmp;
		}

		*len += sprintf(*s + *len, " %s=\"", xml->attr[i]);
		switch_xml_ampencode(xml->attr[i + 1], 0, s, len, max, 1);
		*len += sprintf(*s + *len, "\"");
	}

	for (i = 0; attr[i] && strcmp(attr[i][0], xml->name); i++);
	for (j = 1; attr[i] && attr[i][j]; j += 3) {	// default attributes
		if (!attr[i][j + 1] || switch_xml_attr(xml, attr[i][j]) != attr[i][j + 1])
			continue;			// skip duplicates and non-values
		while (*len + strlen(attr[i][j]) + 8 + (strlen(XML_INDENT) * (*count)) > *max) {	// reallocate s
			char *tmp = (char *)realloc(*s, *max += SWITCH_XML_BUFSIZE);
			if (!tmp) return *s;
			*s = tmp;
		}

		*len += sprintf(*s + *len, " %s=\"", attr[i][j]);
		switch_xml_ampencode(attr[i][j + 1], 0, s, len, max, 1);
		*len += sprintf(*s + *len, "\"");
	}

	*len += sprintf(*s + *len, (xml->child || xml->txt) ? ">" : "/>\n");

	if (xml->child) {
		(*count)++;
		*s = switch_xml_toxml_r(xml->child, s, len, max, 0, attr, count);

	} else {
		*s = switch_xml_ampencode(xml->txt, 0, s, len, max, 0);	//data
	}

	while (*len + strlen(xml->name) + 5 + (strlen(XML_INDENT) * (*count)) > *max) {	// reallocate s
			char *tmp = (char *)realloc(*s, *max += SWITCH_XML_BUFSIZE);
			if (!tmp) return *s;
			*s = tmp;
		}

	if (xml->child || xml->txt) {
		if (*(*s + (*len) - 1) == '\n') {
			for (lcount = 0; lcount < *count; lcount++) {
				*len += sprintf(*s + *len, "%s", XML_INDENT);	// indent
			}
		}
		*len += sprintf(*s + (*len), "</%s>\n", xml->name);	// close tag
	}

	while (txt[off] && off < xml->off)
		off++;					// make sure off is within bounds

	if (xml->ordered) {
		return switch_xml_toxml_r(xml->ordered, s, len, max, off, attr, count);

	} else {
		if (*count > 0)
			(*count)--;
		return switch_xml_ampencode(txt + off, 0, s, len, max, 0);
	}
}

SWITCH_DECLARE(char *) switch_xml_toxml(switch_xml_t xml, switch_bool_t prn_header)
{
	char *s;
	s = (char *)malloc(SWITCH_XML_BUFSIZE);
	return switch_xml_toxml_buf(xml, s, SWITCH_XML_BUFSIZE, 0, prn_header);
}

// converts an switch_xml structure back to xml, returning a string of xml date that
// must be freed
SWITCH_DECLARE(char *) switch_xml_toxml_buf(switch_xml_t xml, char *buf, switch_size_t buflen, switch_size_t offset, switch_bool_t prn_header)
{
	switch_xml_t p = (xml) ? xml->parent : NULL, o = (xml) ? xml->ordered : NULL;
	switch_xml_root_t root = (switch_xml_root_t) xml;
	switch_size_t len = 0, max = buflen;
	char *s, *t, *n, *r;
	int i, j, k;
	uint32_t count = 0;
	
	s = buf;
	assert(s != NULL);
	memset(s, 0, max);
	len += offset;
	if (prn_header) {
		len += sprintf(s + len, "<?xml version=\"1.0\"?>\n");
	}
	
	if (!xml || !xml->name) {
		if (!(r = (char *)realloc(s, len + 1))) {
			abort();
		}
		return r;
	}

	while (root->xml.parent) {
		root = (switch_xml_root_t) root->xml.parent;	// root tag
	}

	for (i = 0; !p && root->pi[i]; i++) {	// pre-root processing instructions
		for (k = 2; root->pi[i][k - 1]; k++);
		for (j = 1; (n = root->pi[i][j]); j++) {
			if (root->pi[i][k][j - 1] == '>') {
				continue;		// not pre-root
			}
			while (len + strlen(t = root->pi[i][0]) + strlen(n) + 7 > max) {
				if (!(r = (char *)realloc(s, max += SWITCH_XML_BUFSIZE))) {
					abort();
				}
				s = r;
			}
			len += sprintf(s + len, "<?%s%s%s?>", t, *n ? " " : "", n);
		}
	}

	xml->parent = xml->ordered = NULL;
	s = switch_xml_toxml_r(xml, &s, &len, &max, 0, root->attr, &count);
	xml->parent = p;
	xml->ordered = o;

	for (i = 0; !p && root->pi[i]; i++) {	// post-root processing instructions
		for (k = 2; root->pi[i][k - 1]; k++);
		for (j = 1; (n = root->pi[i][j]); j++) {
			if (root->pi[i][k][j - 1] == '<') {
				continue;		// not post-root
			}
			while (len + strlen(t = root->pi[i][0]) + strlen(n) + 7 > max) {
				if (!(r = (char *)realloc(s, max += SWITCH_XML_BUFSIZE))) {
					abort();
				}
				s = r;
			}
			len += sprintf(s + len, "\n<?%s%s%s?>", t, *n ? " " : "", n);
		}
	}

	if (!(r = (char *)realloc(s, len + 1))) {
		abort();
	}

	return r;
}

// free the memory allocated for the switch_xml structure
SWITCH_DECLARE(void) switch_xml_free(switch_xml_t xml)
{
	switch_xml_root_t root = (switch_xml_root_t) xml;
	int i, j;
	char **a, *s;


	if (!xml) {
		return;
	}

	if (xml == MAIN_XML_ROOT) {
		switch_thread_rwlock_unlock(RWLOCK);
		return;
	}

	if (xml->free_path) {
		if (!switch_stristr("freeswitch.xml.fsxml", xml->free_path)) {
			if (unlink(xml->free_path) != 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Failed to delete file [%s]\n", xml->free_path);
			}
		}
		switch_safe_free(xml->free_path);
	}

	switch_xml_free(xml->child);
	switch_xml_free(xml->ordered);

	if (!xml->parent) {			// free root tag allocations
		for (i = 10; root->ent[i]; i += 2)	// 0 - 9 are default entities (<>&"')
			if ((s = root->ent[i + 1]) < root->s || s > root->e)
				free(s);
		free(root->ent);		// free list of general entities

		for (i = 0; (a = root->attr[i]); i++) {
			for (j = 1; a[j++]; j += 2)	// free malloced attribute values
				if (a[j] && (a[j] < root->s || a[j] > root->e))
					free(a[j]);
			free(a);
		}
		if (root->attr[0])
			free(root->attr);	// free default attribute list

		for (i = 0; root->pi[i]; i++) {
			for (j = 1; root->pi[i][j]; j++);
			free(root->pi[i][j + 1]);
			free(root->pi[i]);
		}
		if (root->pi[0])
			free(root->pi);		// free processing instructions

		if (root->dynamic == 1)
			free(root->m);		// malloced xml data
#ifdef HAVE_MMAP
		else if (root->len)
			munmap(root->m, root->len);	// mem mapped xml data
#endif // HAVE_MMAP
		if (root->u)
			free(root->u);		// utf8 conversion
	}

	switch_xml_free_attr(xml->attr);	// tag attributes
	if ((xml->flags & SWITCH_XML_TXTM))
		free(xml->txt);			// character content
	if ((xml->flags & SWITCH_XML_NAMEM))
		free(xml->name);		// tag name
	free(xml);
}

// return parser error message or empty string if none
SWITCH_DECLARE(const char *) switch_xml_error(switch_xml_t xml)
{
	while (xml && xml->parent)
		xml = xml->parent;		// find root tag
	return (xml) ? ((switch_xml_root_t) xml)->err : "";
}

// returns a new empty switch_xml structure with the given root tag name
SWITCH_DECLARE(switch_xml_t) switch_xml_new(const char *name)
{
	static const char *ent[] = { "lt;", "&#60;", "gt;", "&#62;", "quot;", "&#34;",
		"apos;", "&#39;", "amp;", "&#38;", NULL
	};
	switch_xml_root_t root = (switch_xml_root_t) memset(malloc(sizeof(struct switch_xml_root)),
														'\0', sizeof(struct switch_xml_root));
	root->xml.name = (char *) name;
	root->cur = &root->xml;
	strcpy(root->err, root->xml.txt = (char *)"");
	root->ent = (char **)memcpy(malloc(sizeof(ent)), ent, sizeof(ent));
	root->attr = root->pi = (char ***) (root->xml.attr = SWITCH_XML_NIL);
	return &root->xml;
}

// inserts an existing tag into an switch_xml structure
SWITCH_DECLARE(switch_xml_t) switch_xml_insert(switch_xml_t xml, switch_xml_t dest, switch_size_t off)
{
	switch_xml_t cur, prev, head;

	xml->next = xml->sibling = xml->ordered = NULL;
	xml->off = off;
	xml->parent = dest;

	if ((head = dest->child)) {	// already have sub tags
		if (head->off <= off) {	// not first subtag
			for (cur = head; cur->ordered && cur->ordered->off <= off; cur = cur->ordered);
			xml->ordered = cur->ordered;
			cur->ordered = xml;
		} else {				// first subtag
			xml->ordered = head;
			dest->child = xml;
		}

		for (cur = head, prev = NULL; cur && strcmp(cur->name, xml->name); prev = cur, cur = cur->sibling);	// find tag type
		if (cur && cur->off <= off) {	// not first of type
			while (cur->next && cur->next->off <= off)
				cur = cur->next;
			xml->next = cur->next;
			cur->next = xml;
		} else {				// first tag of this type
			if (prev && cur)
				prev->sibling = cur->sibling;	// remove old first
			xml->next = cur;	// old first tag is now next
			for (cur = head, prev = NULL; cur && cur->off <= off; prev = cur, cur = cur->sibling);	// new sibling insert point
			xml->sibling = cur;
			if (prev)
				prev->sibling = xml;
		}
	} else
		dest->child = xml;		// only sub tag

	return xml;
}

// Adds a child tag. off is the offset of the child tag relative to the start
// of the parent tag's character content. Returns the child tag
SWITCH_DECLARE(switch_xml_t) switch_xml_add_child(switch_xml_t xml, const char *name, switch_size_t off)
{
	switch_xml_t child;

	if (!xml)
		return NULL;
	child = (switch_xml_t) memset(malloc(sizeof(struct switch_xml)), '\0', sizeof(struct switch_xml));
	child->name = (char *) name;
	child->attr = SWITCH_XML_NIL;
	child->off = off;
	child->parent = xml;
	child->txt = (char *)"";

	return switch_xml_insert(child, xml, off);
}

// sets the character content for the given tag and returns the tag
SWITCH_DECLARE(switch_xml_t) switch_xml_set_txt(switch_xml_t xml, const char *txt)
{
	if (!xml)
		return NULL;
	if (xml->flags & SWITCH_XML_TXTM)
		free(xml->txt);			// existing txt was malloced
	xml->flags &= ~SWITCH_XML_TXTM;
	xml->txt = (char *) txt;
	return xml;
}

// Sets the given tag attribute or adds a new attribute if not found. A value
// of NULL will remove the specified attribute.  Returns the tag given
SWITCH_DECLARE(switch_xml_t) switch_xml_set_attr(switch_xml_t xml, const char *name, const char *value)
{
	int l = 0, c;

	if (!xml)
		return NULL;
	while (xml->attr[l] && strcmp(xml->attr[l], name))
		l += 2;
	if (!xml->attr[l]) {		// not found, add as new attribute
		if (!value)
			return xml;			// nothing to do
		if (xml->attr == SWITCH_XML_NIL) {	// first attribute
			xml->attr = (char **)malloc(4 * sizeof(char *));
			if (!xml->attr) return NULL;
			xml->attr[1] = strdup("");	// empty list of malloced names/vals
		} else {
			char **tmp = (char **)realloc(xml->attr, (l + 4) * sizeof(char *));
			if (!tmp) return xml;
			xml->attr = tmp;
		}

		xml->attr[l] = (char *) name;	// set attribute name
		xml->attr[l + 2] = NULL;	// null terminate attribute list
		xml->attr[l + 3] = (char *)realloc(xml->attr[l + 1], (c = (int) strlen(xml->attr[l + 1])) + 2);
		strcpy(xml->attr[l + 3] + c, " ");	// set name/value as not malloced
		if (xml->flags & SWITCH_XML_DUP)
			xml->attr[l + 3][c] = SWITCH_XML_NAMEM;
	} else if (xml->flags & SWITCH_XML_DUP)
		free((char *) name);	// name was strduped

	for (c = l; xml->attr[c]; c += 2);	// find end of attribute list
	if (xml->attr[c + 1][l / 2] & SWITCH_XML_TXTM)
		free(xml->attr[l + 1]);	//old val
	if (xml->flags & SWITCH_XML_DUP)
		xml->attr[c + 1][l / 2] |= SWITCH_XML_TXTM;
	else
		xml->attr[c + 1][l / 2] &= ~SWITCH_XML_TXTM;

	if (value)
		xml->attr[l + 1] = (char *) value;	// set attribute value
	else {						// remove attribute
		char **tmp;
		if (xml->attr[c + 1][l / 2] & SWITCH_XML_NAMEM)
			free(xml->attr[l]);
		memmove(xml->attr + l, xml->attr + l + 2, (c - l + 2) * sizeof(char *));
		tmp =(char **)realloc(xml->attr, (c + 2) * sizeof(char *));
		if (!tmp) return xml;
		xml->attr = tmp;
		memmove(xml->attr[c + 1] + (l / 2), xml->attr[c + 1] + (l / 2) + 1, (c / 2) - (l / 2));	// fix list of which name/vals are malloced
	}
	xml->flags &= ~SWITCH_XML_DUP;	// clear strdup() flag

	return xml;
}

// sets a flag for the given tag and returns the tag
SWITCH_DECLARE(switch_xml_t) switch_xml_set_flag(switch_xml_t xml, switch_xml_flag_t flag)
{
	if (xml)
		xml->flags |= flag;
	return xml;
}

// removes a tag along with its subtags without freeing its memory
SWITCH_DECLARE(switch_xml_t) switch_xml_cut(switch_xml_t xml)
{
	switch_xml_t cur;

	if (!xml)
		return NULL;			// nothing to do
	if (xml->next)
		xml->next->sibling = xml->sibling;	// patch sibling list

	if (xml->parent) {			// not root tag
		cur = xml->parent->child;	// find head of subtag list
		if (cur == xml)
			xml->parent->child = xml->ordered;	// first subtag
		else {					// not first subtag
			while (cur->ordered != xml)
				cur = cur->ordered;
			cur->ordered = cur->ordered->ordered;	// patch ordered list

			cur = xml->parent->child;	// go back to head of subtag list
			if (strcmp(cur->name, xml->name)) {	// not in first sibling list
				while (strcmp(cur->sibling->name, xml->name))
					cur = cur->sibling;
				if (cur->sibling == xml) {	// first of a sibling list
					cur->sibling = (xml->next) ? xml->next : cur->sibling->sibling;
				} else
					cur = cur->sibling;	// not first of a sibling list
			}

			while (cur->next && cur->next != xml)
				cur = cur->next;
			if (cur->next)
				cur->next = cur->next->next;	// patch next list
		}
	}
	xml->ordered = xml->sibling = xml->next = NULL;	// prevent switch_xml_free() from clobbering ordered list
	return xml;
}

#ifdef WIN32
/* 
 * globbing functions for windows, part of libc on unix, this code was cut and paste from  
 * freebsd lib and distilled a bit to work with windows
 */

/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Guido van Rossum.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define	DOLLAR		'$'
#define	DOT		'.'
#define	EOS		'\0'
#define	LBRACKET	'['
#define	NOT		'!'
#define	QUESTION	'?'
#define	RANGE		'-'
#define	RBRACKET	']'
#define	SEP		'/'
#define WIN_SEP '\\'
#define	STAR		'*'
#define	TILDE		'~'
#define	UNDERSCORE	'_'
#define	LBRACE		'{'
#define	RBRACE		'}'
#define	SLASH		'/'
#define	COMMA		','

#define	M_QUOTE		(char)0x80
#define	M_PROTECT	(char)0x40
#define	M_MASK		(char)0xff
#define	M_ASCII		(char)0x7f

#define	CHAR(c)		((char)((c)&M_ASCII))
#define	META(c)		((char)((c)|M_QUOTE))
#define	M_ALL		META('*')
#define	M_END		META(']')
#define	M_NOT		META('!')
#define	M_ONE		META('?')
#define	M_RNG		META('-')
#define	M_SET		META('[')
#define	ismeta(c)	(((c)&M_QUOTE) != 0)

#ifndef MAXPATHLEN
#define MAXPATHLEN 256
#endif

static int	 compare(const void *, const void *);
static int	 glob0(const char *, glob_t *, size_t *);
static int	 glob1(char *, glob_t *, size_t *);
static int	 glob2(char *, char *, char *, char *, glob_t *, size_t *);
static int	 glob3(char *, char *, char *, char *, char *, glob_t *, size_t *);
static int	 globextend(const char *, glob_t *, size_t *);
static int	 match(char *, char *, char *);

#pragma warning(push)
#pragma warning(disable:4310)

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob)
{
	const unsigned char *patnext;
	size_t limit;
	char c;
	char *bufnext, *bufend, patbuf[MAXPATHLEN];
	
	patnext = (unsigned char *) pattern;
	if (!(flags & GLOB_APPEND)) {
		pglob->gl_pathc = 0;
		pglob->gl_pathv = NULL;
		if (!(flags & GLOB_DOOFFS))
			pglob->gl_offs = 0;
	}
	if (flags & GLOB_LIMIT) {
		limit = pglob->gl_matchc;
		if (limit == 0)
			limit = 9999999;
	} else
		limit = 0;
	pglob->gl_flags = flags & ~GLOB_MAGCHAR;
	pglob->gl_errfunc = errfunc;
	pglob->gl_matchc = 0;
	
	bufnext = patbuf;
	bufend = bufnext + MAXPATHLEN - 1;
	while (bufnext < bufend && (c = *patnext++) != EOS)
		*bufnext++ = c;
	*bufnext = EOS;
	
	return glob0(patbuf, pglob, &limit);
}

/*
 * The main glob() routine: compiles the pattern (optionally processing
 * quotes), calls glob1() to do the real pattern matching, and finally
 * sorts the list (unless unsorted operation is requested).  Returns 0
 * if things went well, nonzero if errors occurred.
 */
static int glob0(const char *pattern, glob_t *pglob, size_t *limit)
{
	const char *qpatnext;
	int c, err;
	size_t oldpathc;
	char *bufnext, patbuf[MAXPATHLEN];
	
	qpatnext = pattern;
	oldpathc = pglob->gl_pathc;
	bufnext = patbuf;
	
	/* We don't need to check for buffer overflow any more. */
	while ((c = *qpatnext++) != EOS) {
		switch (c) {
			case SEP:
				*bufnext++ = WIN_SEP;
				break;
			case LBRACKET:
				c = *qpatnext;
				if (c == NOT)
					++qpatnext;
				if (*qpatnext == EOS ||
					strchr((char *) qpatnext+1, RBRACKET) == NULL) {
					*bufnext++ = LBRACKET;
					if (c == NOT)
						--qpatnext;
					break;
				}
				*bufnext++ = M_SET;
				if (c == NOT)
					*bufnext++ = M_NOT;
				c = *qpatnext++;
				do {
					*bufnext++ = CHAR(c);
					if (*qpatnext == RANGE &&
						(c = qpatnext[1]) != RBRACKET) {
						*bufnext++ = M_RNG;
						*bufnext++ = CHAR(c);
						qpatnext += 2;
					}
				} while ((c = *qpatnext++) != RBRACKET);
				pglob->gl_flags |= GLOB_MAGCHAR;
				*bufnext++ = M_END;
				break;
			case QUESTION:
				pglob->gl_flags |= GLOB_MAGCHAR;
				*bufnext++ = M_ONE;
				break;
			case STAR:
				pglob->gl_flags |= GLOB_MAGCHAR;
				/* collapse adjacent stars to one,
				 * to avoid exponential behavior
				 */
				if (bufnext == patbuf || bufnext[-1] != M_ALL)
					*bufnext++ = M_ALL;
				break;
			default:
				*bufnext++ = CHAR(c);
				break;
		}
	}
	*bufnext = EOS;
	
	if ((err = glob1(patbuf, pglob, limit)) != 0)
		return(err);
	
	/*
	 * If there was no match we are going to append the pattern
	 * if GLOB_NOCHECK was specified or if GLOB_NOMAGIC was specified
	 * and the pattern did not contain any magic characters
	 * GLOB_NOMAGIC is there just for compatibility with csh.
	 */
	if (pglob->gl_pathc == oldpathc) {
		if (((pglob->gl_flags & GLOB_NOCHECK) ||
			 ((pglob->gl_flags & GLOB_NOMAGIC) &&
			  !(pglob->gl_flags & GLOB_MAGCHAR))))
			return(globextend(pattern, pglob, limit));
		else
			return(GLOB_NOMATCH);
	}
	if (!(pglob->gl_flags & GLOB_NOSORT))
		qsort(pglob->gl_pathv + pglob->gl_offs + oldpathc,
			  pglob->gl_pathc - oldpathc, sizeof(char *), compare);
	return(0);
}

static int compare(const void *p, const void *q)
{
	return(strcmp(*(char **)p, *(char **)q));
}

static int glob1(char *pattern, glob_t *pglob, size_t *limit)
{
	char pathbuf[MAXPATHLEN];
	
	/* A null pathname is invalid -- POSIX 1003.1 sect. 2.4. */
	if (*pattern == EOS)
		return(0);
	return(glob2(pathbuf, pathbuf, pathbuf + MAXPATHLEN - 1,
				 pattern, pglob, limit));
}

/*
 * The functions glob2 and glob3 are mutually recursive; there is one level
 * of recursion for each segment in the pattern that contains one or more
 * meta characters.
 */
static int glob2(char *pathbuf, char *pathend, char *pathend_last, char *pattern, glob_t *pglob, size_t *limit)
{
	struct stat sb;
	char *p, *q;
	int anymeta;
	
	/*
	 * Loop over pattern segments until end of pattern or until
	 * segment with meta character found.
	 */
	for (anymeta = 0;;) {
		if (*pattern == EOS) {		/* End of pattern? */
			*pathend = EOS;
			if (stat(pathbuf, &sb))
				return(0);
			
			if (((pglob->gl_flags & GLOB_MARK) && pathend[-1] != SEP && pathend[-1] != WIN_SEP) && (_S_IFDIR & sb.st_mode) ) {
				if (pathend + 1 > pathend_last)
					return (GLOB_ABORTED);
				*pathend++ = WIN_SEP;
				*pathend = EOS;
			}
			++pglob->gl_matchc;
			return(globextend(pathbuf, pglob, limit));
		}
		
		/* Find end of next segment, copy tentatively to pathend. */
		q = pathend;
		p = pattern;
		while (*p != EOS && *p != SEP && *p != WIN_SEP) {
			if (ismeta(*p))
				anymeta = 1;
			if (q + 1 > pathend_last)
				return (GLOB_ABORTED);
			*q++ = *p++;
		}
		
		if (!anymeta) {		/* No expansion, do next segment. */
			pathend = q;
			pattern = p;
			while (*pattern == SEP || *pattern == WIN_SEP) {
				if (pathend + 1 > pathend_last)
					return (GLOB_ABORTED);
				*pathend++ = *pattern++;
			}
		} else			/* Need expansion, recurse. */
			return(glob3(pathbuf, pathend, pathend_last, pattern, p,
						 pglob, limit));
	}
	/* NOTREACHED */
}

static int glob3(char *pathbuf, char *pathend, char *pathend_last, char *pattern, char *restpattern, glob_t *pglob, size_t *limit)
{
	int err;
	apr_dir_t * dirp;
	apr_pool_t * pool;
	
	apr_pool_create(&pool, NULL);
	
	if (pathend > pathend_last)
		return (GLOB_ABORTED);
	*pathend = EOS;
	errno = 0;
	
	if (apr_dir_open (&dirp, pathbuf, pool) != APR_SUCCESS) {
		/* TODO: don't call for ENOENT or ENOTDIR? */
		apr_pool_destroy(pool);
		if (pglob->gl_errfunc) {
			if (pglob->gl_errfunc(pathbuf, errno) ||
			    pglob->gl_flags & GLOB_ERR)
				return (GLOB_ABORTED);
		}
		return(0);
	}
	
	err = 0;
	
	/* Search directory for matching names. */
	while (dirp)
	{
		apr_finfo_t dp;
		unsigned char *sc;
		char *dc;
		
		if (apr_dir_read(&dp, APR_FINFO_NAME, dirp) != APR_SUCCESS)
			break;
		if (!(dp.valid & APR_FINFO_NAME) || !(dp.name) || !strlen(dp.name))
			break;
		
		/* Initial DOT must be matched literally. */
		if (dp.name[0] == DOT && *pattern != DOT)
			continue;
		dc = pathend;
		sc = (unsigned char *) dp.name;
		
		while (dc < pathend_last && (*dc++ = *sc++) != EOS);
		
		if (!match(pathend, pattern, restpattern)) {
			*pathend = EOS;
			continue;
		}
		err = glob2(pathbuf, --dc, pathend_last, restpattern,
					pglob, limit);
		if (err)
			break;
	}
	
	if (dirp) 
		apr_dir_close (dirp);
	apr_pool_destroy(pool);
	return(err);
}


/*
 * Extend the gl_pathv member of a glob_t structure to accommodate a new item,
 * add the new item, and update gl_pathc.
 *
 * This assumes the BSD realloc, which only copies the block when its size
 * crosses a power-of-two boundary; for v7 realloc, this would cause quadratic
 * behavior.
 *
 * Return 0 if new item added, error code if memory couldn't be allocated.
 *
 * Invariant of the glob_t structure:
 *	Either gl_pathc is zero and gl_pathv is NULL; or gl_pathc > 0 and
 *	gl_pathv points to (gl_offs + gl_pathc + 1) items.
 */
static int globextend(const char *path, glob_t *pglob, size_t *limit)
{
	char **pathv;
	char * copy;
	size_t i;
	size_t newsize, len;
	const char *p;
	
	if (*limit && pglob->gl_pathc > *limit) {
		errno = 0;
		return (GLOB_NOSPACE);
	}
	
	newsize = sizeof(*pathv) * (2 + pglob->gl_pathc + pglob->gl_offs);
	pathv = pglob->gl_pathv ?
	realloc((char *)pglob->gl_pathv, newsize) :
	malloc(newsize);
	if (pathv == NULL) {
		if (pglob->gl_pathv) {
			free(pglob->gl_pathv);
			pglob->gl_pathv = NULL;
		}
		return(GLOB_NOSPACE);
	}
	
	if (pglob->gl_pathv == NULL && pglob->gl_offs > 0) {
		/* first time around -- clear initial gl_offs items */
		pathv += pglob->gl_offs;
		for (i = pglob->gl_offs; i-- > 0; )
			*--pathv = NULL;
	}
	pglob->gl_pathv = pathv;
	
	for (p = path; *p++;)
		continue;
	len = (size_t)(p - path);
	if ((copy = malloc(len)) != NULL) {
		memcpy(copy, path, len);
		pathv[pglob->gl_offs + pglob->gl_pathc++] = copy;
	}
	pathv[pglob->gl_offs + pglob->gl_pathc] = NULL;
	return(copy == NULL ? GLOB_NOSPACE : 0);
}

/*
 * pattern matching function for filenames.  Each occurrence of the *
 * pattern causes a recursion level.
 */
static int match(char *name, char *pat, char *patend)
{
	int ok, negate_range;
	char c, k;
	char s1[6];
	
	while (pat < patend) {
		c = *pat++;
		switch (c & M_MASK) {
			case M_ALL:
				if (pat == patend)
					return(1);
				do
					if (match(name, pat, patend))
						return(1);
				while (*name++ != EOS);
				return(0);
			case M_ONE:
				if (*name++ == EOS)
					return(0);
				break;
			case M_SET:
				ok = 0;
				if ((k = *name++) == EOS)
					return(0);
				if ((negate_range = ((*pat & M_MASK) == M_NOT)) != EOS)
					++pat;
				while (((c = *pat++) & M_MASK) != M_END)
					if ((*pat & M_MASK) == M_RNG) {
						memset(s1, 0, sizeof(s1));
						s1[0] = c;
						s1[2] = k;
						s1[4] = pat[1];
						if (strcoll(&s1[0], &s1[2]) <= 0 && strcoll(&s1[2], &s1[4]) <= 0)
							ok = 1;
						pat += 2;
					} else if (c == k)
						ok = 1;
				if (ok == negate_range)
					return(0);
				break;
			default:
				if (*name++ != c)
					return(0);
				break;
		}
	}
	return(*name == EOS);
}

/* Free allocated data belonging to a glob_t structure. */
void globfree(glob_t *pglob)
{
	size_t i;
	char **pp;
	
	if (pglob->gl_pathv != NULL) {
		pp = pglob->gl_pathv + pglob->gl_offs;
		for (i = pglob->gl_pathc; i--; ++pp)
			if (*pp)
				free(*pp);
		free(pglob->gl_pathv);
		pglob->gl_pathv = NULL;
	}
}
#pragma warning(pop)
#endif

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
