/*
 * This file is part of Hubbub.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2007 John-Mark Bell <jmb@netsurf-browser.org>
 */

#include <stdbool.h>
#include <string.h>

#include "utils/utils.h"

#include "tokeniser/entities.h"
#include "tokeniser/tokeniser.h"

/**
 * Table of mappings between Windows-1252 codepoints 128-159 and UCS4
 */
static const uint32_t cp1252Table[32] = {
	0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
	0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178
};

/**
 * Tokeniser states
 */
typedef enum hubbub_tokeniser_state {
	HUBBUB_TOKENISER_STATE_DATA,
	HUBBUB_TOKENISER_STATE_ENTITY_DATA,
	HUBBUB_TOKENISER_STATE_TAG_OPEN,
	HUBBUB_TOKENISER_STATE_CLOSE_TAG_OPEN,
	HUBBUB_TOKENISER_STATE_CLOSE_TAG_MATCH,
	HUBBUB_TOKENISER_STATE_TAG_NAME,
	HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_NAME,
	HUBBUB_TOKENISER_STATE_ATTRIBUTE_NAME,
	HUBBUB_TOKENISER_STATE_AFTER_ATTRIBUTE_NAME,
	HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_VALUE,
	HUBBUB_TOKENISER_STATE_ATTRIBUTE_VALUE_DQ,
	HUBBUB_TOKENISER_STATE_ATTRIBUTE_VALUE_SQ,
	HUBBUB_TOKENISER_STATE_ATTRIBUTE_VALUE_UQ,
	HUBBUB_TOKENISER_STATE_ENTITY_IN_ATTRIBUTE_VALUE,
	HUBBUB_TOKENISER_STATE_BOGUS_COMMENT,
	HUBBUB_TOKENISER_STATE_MARKUP_DECLARATION_OPEN,
	HUBBUB_TOKENISER_STATE_COMMENT_START,
	HUBBUB_TOKENISER_STATE_COMMENT,
	HUBBUB_TOKENISER_STATE_COMMENT_DASH,
	HUBBUB_TOKENISER_STATE_COMMENT_END,
	HUBBUB_TOKENISER_STATE_MATCH_DOCTYPE,
	HUBBUB_TOKENISER_STATE_DOCTYPE,
	HUBBUB_TOKENISER_STATE_BEFORE_DOCTYPE_NAME,
	HUBBUB_TOKENISER_STATE_DOCTYPE_NAME,
	HUBBUB_TOKENISER_STATE_AFTER_DOCTYPE_NAME,
	HUBBUB_TOKENISER_STATE_BOGUS_DOCTYPE,
	HUBBUB_TOKENISER_STATE_NUMBERED_ENTITY,
	HUBBUB_TOKENISER_STATE_NAMED_ENTITY
} hubbub_tokeniser_state;

/**
 * Context for tokeniser
 */
typedef struct hubbub_tokeniser_context {
	hubbub_token_type current_tag_type;	/**< Type of current_tag */
	hubbub_tag current_tag;			/**< Current tag */

	hubbub_string current_comment;		/**< Current comment */

	hubbub_doctype current_doctype;		/**< Current doctype */

	hubbub_string current_chars;		/**< Pending characters */

	hubbub_tokeniser_state prev_state;	/**< Previous state */

	struct {
		hubbub_string tag;		/**< Pending close tag */
	} close_tag_match;

	struct {
		uint32_t count;			/**< Index into "DOCTYPE" */
	} match_doctype;

	struct {
		hubbub_string str;		/**< Pending string */
		uint8_t base;			/**< Base for numeric
						 * entities */
		uint32_t codepoint;		/**< UCS4 codepoint */
		bool had_data;			/**< Whether we read
						 * anything after &#(x)? */
		hubbub_tokeniser_state return_state;	/**< State we were
							 * called from */
		bool complete;			/**< Flag that entity
						 * matching completed */
		bool done_setup;		/**< Flag that match setup
						 * has completed */
		void *context;			/**< Context for named
						 * entity search */
		size_t prev_len;		/**< Previous byte length
						 * of str */
	} match_entity;

	struct {
		uint32_t line;			/**< Current line of input */
		uint32_t col;			/**< Current character in
						 * line */
	} position;
} hubbub_tokeniser_context;

/**
 * Tokeniser data structure
 */
struct hubbub_tokeniser {
	hubbub_tokeniser_state state;	/**< Current tokeniser state */
	hubbub_content_model content_model;	/**< Current content
						 * model flag */

	hubbub_inputstream *input;	/**< Input stream */

	const uint8_t *input_buffer;	/**< Start of input stream's buffer */
	size_t input_buffer_len;	/**< Length of input buffer */

	hubbub_tokeniser_context context;	/**< Tokeniser context */

	hubbub_token_handler token_handler;
	void *token_pw;

	hubbub_buffer_handler buffer_handler;
	void *buffer_pw;

	hubbub_error_handler error_handler;
	void *error_pw;

	hubbub_alloc alloc;		/**< Memory (de)allocation function */
	void *alloc_pw;			/**< Client private data */
};

static bool hubbub_tokeniser_handle_data(hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_entity_data(hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_tag_open(hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_close_tag_open(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_close_tag_match(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_tag_name(hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_before_attribute_name(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_attribute_name(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_after_attribute_name(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_before_attribute_value(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_attribute_value_dq(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_attribute_value_sq(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_attribute_value_uq(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_entity_in_attribute_value(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_bogus_comment(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_markup_declaration_open(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_comment_start(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_comment(hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_comment_dash(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_comment_end(hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_match_doctype(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_doctype(hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_before_doctype_name(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_doctype_name(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_after_doctype_name(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_bogus_doctype(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_consume_entity(hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_numbered_entity(
		hubbub_tokeniser *tokeniser);
static bool hubbub_tokeniser_handle_named_entity(
		hubbub_tokeniser *tokeniser);
static void hubbub_tokeniser_buffer_moved_handler(const uint8_t *buffer,
		size_t len, void *pw);
static void hubbub_tokeniser_emit_token(hubbub_tokeniser *tokeniser,
		hubbub_token *token);

/**
 * Create a hubbub tokeniser
 *
 * \param input  Input stream instance
 * \param alloc  Memory (de)allocation function
 * \param pw     Pointer to client-specific private data (may be NULL)
 * \return Pointer to tokeniser instance, or NULL on failure
 */
hubbub_tokeniser *hubbub_tokeniser_create(hubbub_inputstream *input,
		hubbub_alloc alloc, void *pw)
{
	hubbub_tokeniser *tok;

	if (input == NULL || alloc == NULL)
		return NULL;

	tok = alloc(NULL, sizeof(hubbub_tokeniser), pw);
	if (tok == NULL)
		return NULL;

	tok->state = HUBBUB_TOKENISER_STATE_DATA;
	tok->content_model = HUBBUB_CONTENT_MODEL_PCDATA;

	tok->input = input;
	tok->input_buffer = NULL;
	tok->input_buffer_len = 0;

	tok->token_handler = NULL;
	tok->token_pw = NULL;

	tok->buffer_handler = NULL;
	tok->buffer_pw = NULL;

	tok->error_handler = NULL;
	tok->error_pw = NULL;

	tok->alloc = alloc;
	tok->alloc_pw = pw;

	if (hubbub_inputstream_register_movehandler(input,
			hubbub_tokeniser_buffer_moved_handler, tok) !=
			HUBBUB_OK) {
		alloc(tok, 0, pw);
		return NULL;
	}

	memset(&tok->context, 0, sizeof(hubbub_tokeniser_context));

	return tok;
}

/**
 * Destroy a hubbub tokeniser
 *
 * \param tokeniser  The tokeniser instance to destroy
 */
void hubbub_tokeniser_destroy(hubbub_tokeniser *tokeniser)
{
	if (tokeniser == NULL)
		return;

	hubbub_inputstream_deregister_movehandler(tokeniser->input,
			hubbub_tokeniser_buffer_moved_handler, tokeniser);

	if (tokeniser->context.current_tag.attributes != NULL) {
		tokeniser->alloc(tokeniser->context.current_tag.attributes,
				0, tokeniser->alloc_pw);
	}

	tokeniser->alloc(tokeniser, 0, tokeniser->alloc_pw);
}

/**
 * Configure a hubbub tokeniser
 *
 * \param tokeniser  The tokeniser instance to configure
 * \param type       The option type to set
 * \param params     Option-specific parameters
 * \return HUBBUB_OK on success, appropriate error otherwise
 */
hubbub_error hubbub_tokeniser_setopt(hubbub_tokeniser *tokeniser,
		hubbub_tokeniser_opttype type,
		hubbub_tokeniser_optparams *params)
{
	if (tokeniser == NULL || params == NULL)
		return HUBBUB_BADPARM;

	switch (type) {
	case HUBBUB_TOKENISER_TOKEN_HANDLER:
		tokeniser->token_handler = params->token_handler.handler;
		tokeniser->token_pw = params->token_handler.pw;
		break;
	case HUBBUB_TOKENISER_BUFFER_HANDLER:
		tokeniser->buffer_handler = params->buffer_handler.handler;
		tokeniser->buffer_pw = params->buffer_handler.pw;
		tokeniser->buffer_handler(tokeniser->input_buffer,
				tokeniser->input_buffer_len,
				tokeniser->buffer_pw);
		break;
	case HUBBUB_TOKENISER_ERROR_HANDLER:
		tokeniser->error_handler = params->error_handler.handler;
		tokeniser->error_pw = params->error_handler.pw;
		break;
	case HUBBUB_TOKENISER_CONTENT_MODEL:
		tokeniser->content_model = params->content_model.model;
		break;
	}

	return HUBBUB_OK;
}

/**
 * Process remaining data in the input stream
 *
 * \param tokeniser  The tokeniser instance to invoke
 * \return HUBBUB_OK on success, appropriate error otherwise
 */
hubbub_error hubbub_tokeniser_run(hubbub_tokeniser *tokeniser)
{
	bool cont = true;

	if (tokeniser == NULL)
		return HUBBUB_BADPARM;

	while (cont) {
		switch (tokeniser->state) {
		case HUBBUB_TOKENISER_STATE_DATA:
			cont = hubbub_tokeniser_handle_data(tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_ENTITY_DATA:
			cont = hubbub_tokeniser_handle_entity_data(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_TAG_OPEN:
			cont = hubbub_tokeniser_handle_tag_open(tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_CLOSE_TAG_OPEN:
			cont = hubbub_tokeniser_handle_close_tag_open(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_CLOSE_TAG_MATCH:
			cont = hubbub_tokeniser_handle_close_tag_match(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_TAG_NAME:
			cont = hubbub_tokeniser_handle_tag_name(tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_NAME:
			cont = hubbub_tokeniser_handle_before_attribute_name(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_ATTRIBUTE_NAME:
			cont = hubbub_tokeniser_handle_attribute_name(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_AFTER_ATTRIBUTE_NAME:
			cont = hubbub_tokeniser_handle_after_attribute_name(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_VALUE:
			cont = hubbub_tokeniser_handle_before_attribute_value(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_ATTRIBUTE_VALUE_DQ:
			cont = hubbub_tokeniser_handle_attribute_value_dq(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_ATTRIBUTE_VALUE_SQ:
			cont = hubbub_tokeniser_handle_attribute_value_sq(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_ATTRIBUTE_VALUE_UQ:
			cont = hubbub_tokeniser_handle_attribute_value_uq(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_ENTITY_IN_ATTRIBUTE_VALUE:
			cont = hubbub_tokeniser_handle_entity_in_attribute_value(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_BOGUS_COMMENT:
			cont = hubbub_tokeniser_handle_bogus_comment(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_MARKUP_DECLARATION_OPEN:
			cont = hubbub_tokeniser_handle_markup_declaration_open(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_COMMENT_START:
			cont = hubbub_tokeniser_handle_comment_start(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_COMMENT:
			cont = hubbub_tokeniser_handle_comment(tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_COMMENT_DASH:
			cont = hubbub_tokeniser_handle_comment_dash(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_COMMENT_END:
			cont = hubbub_tokeniser_handle_comment_end(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_MATCH_DOCTYPE:
			cont = hubbub_tokeniser_handle_match_doctype(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_DOCTYPE:
			cont = hubbub_tokeniser_handle_doctype(tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_BEFORE_DOCTYPE_NAME:
			cont = hubbub_tokeniser_handle_before_doctype_name(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_DOCTYPE_NAME:
			cont = hubbub_tokeniser_handle_doctype_name(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_AFTER_DOCTYPE_NAME:
			cont = hubbub_tokeniser_handle_after_doctype_name(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_BOGUS_DOCTYPE:
			cont = hubbub_tokeniser_handle_bogus_doctype(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_NUMBERED_ENTITY:
			cont = hubbub_tokeniser_handle_numbered_entity(
					tokeniser);
			break;
		case HUBBUB_TOKENISER_STATE_NAMED_ENTITY:
			cont = hubbub_tokeniser_handle_named_entity(
					tokeniser);
			break;
		}
	}

	return HUBBUB_OK;
}

bool hubbub_tokeniser_handle_data(hubbub_tokeniser *tokeniser)
{
	hubbub_token token;
	uint32_t c;

	/* Clear current characters */
	tokeniser->context.current_chars.data_off = 0;
	tokeniser->context.current_chars.len = 0;

	while ((c = hubbub_inputstream_peek(tokeniser->input)) !=
			HUBBUB_INPUTSTREAM_EOF &&
			c != HUBBUB_INPUTSTREAM_OOD) {
		if (c == '&' && (tokeniser->content_model ==
					HUBBUB_CONTENT_MODEL_PCDATA ||
				tokeniser->content_model ==
					HUBBUB_CONTENT_MODEL_RCDATA)) {
			tokeniser->state =
					HUBBUB_TOKENISER_STATE_ENTITY_DATA;
			/* Don't eat the '&'; it'll be handled by
			 * entity consumption */
			break;
		} else if (c == '<' && tokeniser->content_model !=
				HUBBUB_CONTENT_MODEL_PLAINTEXT) {
			if (tokeniser->context.current_chars.len > 0) {
				/* Emit any pending characters */
				token.type = HUBBUB_TOKEN_CHARACTER;
				token.data.character =
					tokeniser->context.current_chars;

				hubbub_tokeniser_emit_token(tokeniser,
						&token);
			}

			/* Buffer '<' */
			tokeniser->context.current_chars.data_off =
				hubbub_inputstream_cur_pos(tokeniser->input,
					&tokeniser->context.current_chars.len);

			tokeniser->state = HUBBUB_TOKENISER_STATE_TAG_OPEN;
			hubbub_inputstream_advance(tokeniser->input);
			break;
		} else {
			uint32_t pos;
			size_t len;

			/* Accumulate characters into buffer */
			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);

			if (tokeniser->context.current_chars.len == 0) {
				tokeniser->context.current_chars.data_off =
						pos;
			}
			tokeniser->context.current_chars.len++;

			hubbub_inputstream_advance(tokeniser->input);
		}
	}

	if (tokeniser->state != HUBBUB_TOKENISER_STATE_TAG_OPEN &&
			tokeniser->context.current_chars.len > 0) {
		/* Emit any pending characters */
		token.type = HUBBUB_TOKEN_CHARACTER;
		token.data.character = tokeniser->context.current_chars;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->context.current_chars.data_off = 0;
		tokeniser->context.current_chars.len = 0;
	}

	if (c == HUBBUB_INPUTSTREAM_EOF) {
		token.type = HUBBUB_TOKEN_EOF;

		hubbub_tokeniser_emit_token(tokeniser, &token);
	}

	return (c != HUBBUB_INPUTSTREAM_EOF && c != HUBBUB_INPUTSTREAM_OOD);
}

bool hubbub_tokeniser_handle_entity_data(hubbub_tokeniser *tokeniser)
{
	if (tokeniser->context.match_entity.complete == false) {
		return hubbub_tokeniser_consume_entity(tokeniser);
	} else {
		hubbub_token token;
		uint32_t c = hubbub_inputstream_peek(tokeniser->input);

		if (c == HUBBUB_INPUTSTREAM_OOD ||
				c == HUBBUB_INPUTSTREAM_EOF) {
			/* Should never happen */
			abort();
		}

		/* Emit character */
		token.type = HUBBUB_TOKEN_CHARACTER;
		token.data.character.data_off =
				hubbub_inputstream_cur_pos(tokeniser->input,
						&token.data.character.len);

		hubbub_tokeniser_emit_token(tokeniser, &token);

		/* Reset for next time */
		tokeniser->context.match_entity.complete = false;

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_tag_open(hubbub_tokeniser *tokeniser)
{
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t pos;
	size_t len;

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (tokeniser->content_model == HUBBUB_CONTENT_MODEL_RCDATA ||
			tokeniser->content_model ==
					HUBBUB_CONTENT_MODEL_CDATA) {
		if (c == '/') {
			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);
			tokeniser->context.current_chars.len += len;

			tokeniser->state =
				HUBBUB_TOKENISER_STATE_CLOSE_TAG_OPEN;

			hubbub_inputstream_advance(tokeniser->input);
		} else {
			hubbub_token token;

			/* Emit '<' */
			token.type = HUBBUB_TOKEN_CHARACTER;
			token.data.character =
					tokeniser->context.current_chars;

			hubbub_tokeniser_emit_token(tokeniser, &token);

			tokeniser->state =
				HUBBUB_TOKENISER_STATE_DATA;
		}
	} else if (tokeniser->content_model == HUBBUB_CONTENT_MODEL_PCDATA) {
		if (c == '!') {
			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);

			tokeniser->context.current_chars.len += len;

			tokeniser->state =
				HUBBUB_TOKENISER_STATE_MARKUP_DECLARATION_OPEN;
			hubbub_inputstream_advance(tokeniser->input);
		} else if (c == '/') {
			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);

			tokeniser->context.current_chars.len += len;

			tokeniser->state =
				HUBBUB_TOKENISER_STATE_CLOSE_TAG_OPEN;
			hubbub_inputstream_advance(tokeniser->input);
		} else if ('A' <= c && c <= 'Z') {
			hubbub_inputstream_lowercase(tokeniser->input);

			tokeniser->context.current_tag_type =
					HUBBUB_TOKEN_START_TAG;

			ctag->name.data_off =
				hubbub_inputstream_cur_pos(tokeniser->input,
				&ctag->name.len);
			ctag->n_attributes = 0;

			tokeniser->state =
				HUBBUB_TOKENISER_STATE_TAG_NAME;
			hubbub_inputstream_advance(tokeniser->input);
		} else if ('a' <= c && c <= 'z') {
			tokeniser->context.current_tag_type =
					HUBBUB_TOKEN_START_TAG;

			ctag->name.data_off =
				hubbub_inputstream_cur_pos(tokeniser->input,
				&ctag->name.len);
			ctag->n_attributes = 0;

			tokeniser->state =
				HUBBUB_TOKENISER_STATE_TAG_NAME;
			hubbub_inputstream_advance(tokeniser->input);
		} else if (c == '>') {
			hubbub_token token;

			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);
			tokeniser->context.current_chars.len += len;

			/* Emit "<>" */
			token.type = HUBBUB_TOKEN_CHARACTER;
			token.data.character =
					tokeniser->context.current_chars;

			hubbub_tokeniser_emit_token(tokeniser, &token);

			tokeniser->state =
				HUBBUB_TOKENISER_STATE_DATA;

			hubbub_inputstream_advance(tokeniser->input);
		} else if (c == '?') {
			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);
			tokeniser->context.current_chars.len += len;

			tokeniser->context.current_comment.data_off = pos;
			tokeniser->context.current_comment.len = len;
			tokeniser->state =
				HUBBUB_TOKENISER_STATE_BOGUS_COMMENT;
			hubbub_inputstream_advance(tokeniser->input);
		} else {
			hubbub_token token;

			/* Emit '<' */
			token.type = HUBBUB_TOKEN_CHARACTER;
			token.data.character =
					tokeniser->context.current_chars;

			hubbub_tokeniser_emit_token(tokeniser, &token);

			tokeniser->state =
				HUBBUB_TOKENISER_STATE_DATA;
		}
	}

	return true;
}

bool hubbub_tokeniser_handle_close_tag_open(hubbub_tokeniser *tokeniser)
{
	if (tokeniser->content_model == HUBBUB_CONTENT_MODEL_RCDATA ||
			tokeniser->content_model ==
					HUBBUB_CONTENT_MODEL_CDATA) {
		tokeniser->context.close_tag_match.tag.len = 0;
		tokeniser->state = HUBBUB_TOKENISER_STATE_CLOSE_TAG_MATCH;
	} else if (tokeniser->content_model == HUBBUB_CONTENT_MODEL_PCDATA) {
		hubbub_tag *ctag = &tokeniser->context.current_tag;
		uint32_t c = hubbub_inputstream_peek(tokeniser->input);
		uint32_t pos;
		size_t len;

		if ('A' <= c && c <= 'Z') {
			hubbub_inputstream_lowercase(tokeniser->input);

			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);

			tokeniser->context.current_tag_type =
					HUBBUB_TOKEN_END_TAG;
			ctag->name.data_off = pos;
			ctag->name.len = len;
			ctag->n_attributes = 0;

			tokeniser->state = HUBBUB_TOKENISER_STATE_TAG_NAME;
			hubbub_inputstream_advance(tokeniser->input);
		} else if ('a' <= c && c <= 'z') {
			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);

			tokeniser->context.current_tag_type =
					HUBBUB_TOKEN_END_TAG;
			ctag->name.data_off = pos;
			ctag->name.len = len;
			ctag->n_attributes = 0;

			tokeniser->state = HUBBUB_TOKENISER_STATE_TAG_NAME;
			hubbub_inputstream_advance(tokeniser->input);
		} else if (c == '>') {
			tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
			hubbub_inputstream_advance(tokeniser->input);
		} else if (c == HUBBUB_INPUTSTREAM_EOF) {
			hubbub_token token;

			/* Emit "</" */
			token.type = HUBBUB_TOKEN_CHARACTER;
			token.data.character =
					tokeniser->context.current_chars;

			hubbub_tokeniser_emit_token(tokeniser, &token);

			tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		} else if (c != HUBBUB_INPUTSTREAM_OOD) {
			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);

			tokeniser->context.current_comment.data_off = pos;
			tokeniser->context.current_comment.len = len;

			tokeniser->state =
				HUBBUB_TOKENISER_STATE_BOGUS_COMMENT;
			hubbub_inputstream_advance(tokeniser->input);
		} else {
			/* Out of data */
			return false;
		}
	}

	return true;
}

bool hubbub_tokeniser_handle_close_tag_match(hubbub_tokeniser *tokeniser)
{
	hubbub_tokeniser_context *ctx = &tokeniser->context;
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t c = 0;

	while (ctx->close_tag_match.tag.len < ctag->name.len &&
			(c = hubbub_inputstream_peek(tokeniser->input)) !=
			HUBBUB_INPUTSTREAM_EOF &&
			c != HUBBUB_INPUTSTREAM_OOD) {
		/* Match last open tag */
		uint32_t off;
		size_t len;

		off = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		if (ctx->close_tag_match.tag.len == 0) {
			ctx->close_tag_match.tag.data_off = off;
			ctx->close_tag_match.tag.len = len;
		} else {
			ctx->close_tag_match.tag.len += len;
		}

		hubbub_inputstream_advance(tokeniser->input);

		if (ctx->close_tag_match.tag.len > ctag->name.len ||
			(ctx->close_tag_match.tag.len == ctag->name.len &&
				hubbub_inputstream_compare_range_ci(
					tokeniser->input,
					ctag->name.data_off,
					ctx->close_tag_match.tag.data_off,
					ctag->name.len) != 0)) {
			hubbub_token token;

			/* Rewind input stream to start of tag name */
			if (hubbub_inputstream_rewind(tokeniser->input,
					ctx->close_tag_match.tag.len) !=
					HUBBUB_OK)
				abort();

			/* Emit "</" */
			token.type = HUBBUB_TOKEN_CHARACTER;
			token.data.character =
					tokeniser->context.current_chars;

			hubbub_tokeniser_emit_token(tokeniser, &token);

			tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;

			return true;
		} else if (ctx->close_tag_match.tag.len == ctag->name.len &&
				hubbub_inputstream_compare_range_ci(
					tokeniser->input,
					ctag->name.data_off,
					ctx->close_tag_match.tag.data_off,
					ctag->name.len) == 0) {
			/* Matched => stop searching */
			break;
		}
	}

	if (c == HUBBUB_INPUTSTREAM_OOD) {
		/* Need more data */
		return false;
	}

	if (c == HUBBUB_INPUTSTREAM_EOF) {
		/* Ran out of data - parse error */
		hubbub_token token;

		/* Rewind input stream to start of tag name */
		if (hubbub_inputstream_rewind(tokeniser->input,
				ctx->close_tag_match.tag.len) != HUBBUB_OK)
			abort();

		/* Emit "</" */
		token.type = HUBBUB_TOKEN_CHARACTER;
		token.data.character = tokeniser->context.current_chars;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;

		return true;
	}

	/* Match following char */
	c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD) {
		/* Need more data */
		return false;
	}

	/* Rewind input stream to start of tag name */
	if (hubbub_inputstream_rewind(tokeniser->input,
			ctx->close_tag_match.tag.len) != HUBBUB_OK)
		abort();

	/* Check that following char was valid */
	if (c != '\t' && c != '\n' && c != '\v' && c != '\f' &&
			c != ' ' && c != '>' && c != '/' && c != '<' &&
			c != HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit "</" */
		token.type = HUBBUB_TOKEN_CHARACTER;
		token.data.character = tokeniser->context.current_chars;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;

		return true;
	}

	/* Switch the content model back to PCDATA */
	tokeniser->content_model = HUBBUB_CONTENT_MODEL_PCDATA;

	/* Finally, transition back to close tag open state */
	tokeniser->state = HUBBUB_TOKENISER_STATE_CLOSE_TAG_OPEN;

	return true;
}

bool hubbub_tokeniser_handle_tag_name(hubbub_tokeniser *tokeniser)
{
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == ' ') {
		tokeniser->state =
			HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_NAME;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '>') {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if ('A' <= c && c <= 'Z') {
		uint32_t pos;
		size_t len;

		hubbub_inputstream_lowercase(tokeniser->input);

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		ctag->name.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '<' || c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else if (c == '/') {
		/** \todo permitted slash */
		tokeniser->state =
			HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_NAME;
		hubbub_inputstream_advance(tokeniser->input);
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		ctag->name.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_before_attribute_name(
		hubbub_tokeniser *tokeniser)
{
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == ' ') {
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '>') {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if ('A' <= c && c <= 'Z') {
		uint32_t pos;
		size_t len;
		hubbub_attribute *attr;

		hubbub_inputstream_lowercase(tokeniser->input);

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		attr = tokeniser->alloc(ctag->attributes,
				(ctag->n_attributes + 1) *
					sizeof(hubbub_attribute),
				tokeniser->alloc_pw);
		if (attr == NULL) {
			/** \todo handle memory exhaustion */
		}

		ctag->attributes = attr;

		attr[ctag->n_attributes].name.data_off = pos;
		attr[ctag->n_attributes].name.len = len;
		attr[ctag->n_attributes].value.data_off = 0;
		attr[ctag->n_attributes].value.len = 0;

		ctag->n_attributes++;

		tokeniser->state = HUBBUB_TOKENISER_STATE_ATTRIBUTE_NAME;

		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '/') {
		/** \todo permitted slash */
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '<' || c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;
		hubbub_attribute *attr;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		attr = tokeniser->alloc(ctag->attributes,
				(ctag->n_attributes + 1) *
					sizeof(hubbub_attribute),
				tokeniser->alloc_pw);
		if (attr == NULL) {
			/** \todo handle memory exhaustion */
		}

		ctag->attributes = attr;

		attr[ctag->n_attributes].name.data_off = pos;
		attr[ctag->n_attributes].name.len = len;
		attr[ctag->n_attributes].value.data_off = 0;
		attr[ctag->n_attributes].value.len = 0;

		ctag->n_attributes++;

		tokeniser->state = HUBBUB_TOKENISER_STATE_ATTRIBUTE_NAME;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_attribute_name(hubbub_tokeniser *tokeniser)
{
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == ' ') {
		tokeniser->state =
				HUBBUB_TOKENISER_STATE_AFTER_ATTRIBUTE_NAME;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '=') {
		tokeniser->state =
			HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_VALUE;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '>') {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if ('A' <= c && c <= 'Z') {
		uint32_t pos;
		size_t len;

		hubbub_inputstream_lowercase(tokeniser->input);

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		ctag->attributes[ctag->n_attributes - 1].name.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '/') {
		/** \todo permitted slash */
		tokeniser->state =
				HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_NAME;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '<' || c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		ctag->attributes[ctag->n_attributes - 1].name.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_after_attribute_name(
		hubbub_tokeniser *tokeniser)
{
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == ' ') {
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '=') {
		tokeniser->state =
			HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_VALUE;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '>') {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if ('A' <= c && c <= 'Z') {
		uint32_t pos;
		size_t len;
		hubbub_attribute *attr;

		hubbub_inputstream_lowercase(tokeniser->input);

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		attr = tokeniser->alloc(ctag->attributes,
				(ctag->n_attributes + 1) *
					sizeof(hubbub_attribute),
				tokeniser->alloc_pw);
		if (attr == NULL) {
			/** \todo handle memory exhaustion */
		}

		ctag->attributes = attr;

		attr[ctag->n_attributes].name.data_off = pos;
		attr[ctag->n_attributes].name.len = len;
		attr[ctag->n_attributes].value.data_off = 0;
		attr[ctag->n_attributes].value.len = 0;

		ctag->n_attributes++;

		tokeniser->state = HUBBUB_TOKENISER_STATE_ATTRIBUTE_NAME;

		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '/') {
		/** \todo permitted slash */
		tokeniser->state =
				HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_NAME;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '<' || c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;
		hubbub_attribute *attr;

		hubbub_inputstream_lowercase(tokeniser->input);

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		attr = tokeniser->alloc(ctag->attributes,
				(ctag->n_attributes + 1) *
					sizeof(hubbub_attribute),
				tokeniser->alloc_pw);
		if (attr == NULL) {
			/** \todo handle memory exhaustion */
		}

		ctag->attributes = attr;

		attr[ctag->n_attributes].name.data_off = pos;
		attr[ctag->n_attributes].name.len = len;
		attr[ctag->n_attributes].value.data_off = 0;
		attr[ctag->n_attributes].value.len = 0;

		ctag->n_attributes++;

		tokeniser->state = HUBBUB_TOKENISER_STATE_ATTRIBUTE_NAME;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_before_attribute_value(
		hubbub_tokeniser *tokeniser)
{
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == ' ') {
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '"') {
		tokeniser->state = HUBBUB_TOKENISER_STATE_ATTRIBUTE_VALUE_DQ;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '&') {
		tokeniser->state = HUBBUB_TOKENISER_STATE_ATTRIBUTE_VALUE_UQ;
	} else if (c == '\'') {
		tokeniser->state = HUBBUB_TOKENISER_STATE_ATTRIBUTE_VALUE_SQ;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '>') {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '<' || c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		ctag->attributes[ctag->n_attributes - 1].value.data_off = pos;
		ctag->attributes[ctag->n_attributes - 1].value.len = len;

		tokeniser->state = HUBBUB_TOKENISER_STATE_ATTRIBUTE_VALUE_UQ;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_attribute_value_dq(hubbub_tokeniser *tokeniser)
{
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '"') {
		tokeniser->state =
				HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_NAME;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '&') {
		tokeniser->context.prev_state = tokeniser->state;
		tokeniser->state =
			HUBBUB_TOKENISER_STATE_ENTITY_IN_ATTRIBUTE_VALUE;
		/* Don't eat the '&'; entity consumption handles this */
	} else if (c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		if (ctag->attributes[ctag->n_attributes - 1].value.len == 0) {
			ctag->attributes[ctag->n_attributes - 1].value.data_off =
					pos;
		}

		ctag->attributes[ctag->n_attributes - 1].value.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_attribute_value_sq(hubbub_tokeniser *tokeniser)
{
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\'') {
		tokeniser->state =
				HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_NAME;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '&') {
		tokeniser->context.prev_state = tokeniser->state;
		tokeniser->state =
			HUBBUB_TOKENISER_STATE_ENTITY_IN_ATTRIBUTE_VALUE;
		/* Don't eat the '&'; entity consumption handles this */
	} else if (c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		if (ctag->attributes[ctag->n_attributes - 1].value.len == 0) {
			ctag->attributes[ctag->n_attributes - 1].value.data_off =
					pos;
		}

		ctag->attributes[ctag->n_attributes - 1].value.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_attribute_value_uq(hubbub_tokeniser *tokeniser)
{
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == ' ') {
		tokeniser->state =
				HUBBUB_TOKENISER_STATE_BEFORE_ATTRIBUTE_NAME;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '&') {
		tokeniser->context.prev_state = tokeniser->state;
		tokeniser->state =
			HUBBUB_TOKENISER_STATE_ENTITY_IN_ATTRIBUTE_VALUE;
		/* Don't eat the '&'; entity consumption handles this */
	} else if (c == '>') {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '<' || c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit current tag */
		token.type = tokeniser->context.current_tag_type;
		token.data.tag = tokeniser->context.current_tag;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		if (ctag->attributes[ctag->n_attributes - 1].value.len == 0) {
			ctag->attributes[ctag->n_attributes - 1].value.data_off =
					pos;
		}

		ctag->attributes[ctag->n_attributes - 1].value.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_entity_in_attribute_value(
		hubbub_tokeniser *tokeniser)
{
	hubbub_tag *ctag = &tokeniser->context.current_tag;
	uint32_t pos;
	size_t len;

	if (tokeniser->context.match_entity.complete == false) {
		return hubbub_tokeniser_consume_entity(tokeniser);
	} else {
		uint32_t c = hubbub_inputstream_peek(tokeniser->input);

		if (c == HUBBUB_INPUTSTREAM_OOD ||
				c == HUBBUB_INPUTSTREAM_EOF) {
			/* Should never happen */
			abort();
		}

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		if (ctag->attributes[ctag->n_attributes - 1].value.len == 0) {
			ctag->attributes[ctag->n_attributes - 1].value.data_off =
					pos;
		}

		ctag->attributes[ctag->n_attributes - 1].value.len += len;

		/* Reset for next time */
		tokeniser->context.match_entity.complete = false;

		/* And back to the previous state */
		tokeniser->state = tokeniser->context.prev_state;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_bogus_comment(hubbub_tokeniser *tokeniser)
{
	hubbub_token token;
	uint32_t c;

	while ((c = hubbub_inputstream_peek(tokeniser->input)) !=
			HUBBUB_INPUTSTREAM_EOF &&
			c != HUBBUB_INPUTSTREAM_OOD) {
		uint32_t pos;
		size_t len;

		if (c == '>') {
			hubbub_inputstream_advance(tokeniser->input);
			break;
		}

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		if (tokeniser->context.current_comment.len == 0)
			tokeniser->context.current_comment.data_off = pos;
		tokeniser->context.current_comment.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	}

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	/* Emit comment */
	token.type = HUBBUB_TOKEN_COMMENT;
	token.data.comment = tokeniser->context.current_comment;

	hubbub_tokeniser_emit_token(tokeniser, &token);

	tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;

	return true;
}

bool hubbub_tokeniser_handle_markup_declaration_open(
		hubbub_tokeniser *tokeniser)
{
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '-') {
		tokeniser->state = HUBBUB_TOKENISER_STATE_COMMENT_START;
		hubbub_inputstream_advance(tokeniser->input);
	} else if ((c & ~0x20) == 'D') {
		hubbub_inputstream_uppercase(tokeniser->input);
		tokeniser->context.match_doctype.count = 1;
		tokeniser->state = HUBBUB_TOKENISER_STATE_MATCH_DOCTYPE;
		hubbub_inputstream_advance(tokeniser->input);
	} else {
		tokeniser->context.current_comment.data_off = 0;
		tokeniser->context.current_comment.len = 0;

		tokeniser->state = HUBBUB_TOKENISER_STATE_BOGUS_COMMENT;
	}

	return true;
}

bool hubbub_tokeniser_handle_comment_start(hubbub_tokeniser *tokeniser)
{
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	tokeniser->context.current_comment.data_off = 0;
	tokeniser->context.current_comment.len = 0;


	if (c == '-') {
		tokeniser->state = HUBBUB_TOKENISER_STATE_COMMENT;
		hubbub_inputstream_advance(tokeniser->input);
	} else {
		hubbub_inputstream_push_back(tokeniser->input, '-');
		tokeniser->state = HUBBUB_TOKENISER_STATE_BOGUS_COMMENT;
	}

	return true;
}

bool hubbub_tokeniser_handle_comment(hubbub_tokeniser *tokeniser)
{
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '-') {
		tokeniser->state = HUBBUB_TOKENISER_STATE_COMMENT_DASH;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit comment */
		token.type = HUBBUB_TOKEN_COMMENT;
		token.data.comment = tokeniser->context.current_comment;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		if (tokeniser->context.current_comment.len == 0)
			tokeniser->context.current_comment.data_off = pos;
		tokeniser->context.current_comment.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_comment_dash(hubbub_tokeniser *tokeniser)
{
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '-') {
		tokeniser->state = HUBBUB_TOKENISER_STATE_COMMENT_END;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit comment */
		token.type = HUBBUB_TOKEN_COMMENT;
		token.data.comment = tokeniser->context.current_comment;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		if (tokeniser->context.current_comment.len == 0) {
			tokeniser->context.current_comment.data_off = pos;
		} else {
			/* Need to do this to get length of '-' */
			len += pos -
				tokeniser->context.current_comment.data_off;
		}

		tokeniser->context.current_comment.len = len;

		tokeniser->state = HUBBUB_TOKENISER_STATE_COMMENT;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_comment_end(hubbub_tokeniser *tokeniser)
{
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '>') {
		hubbub_token token;

		/* Emit comment */
		token.type = HUBBUB_TOKEN_COMMENT;
		token.data.comment = tokeniser->context.current_comment;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '-') {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		if (tokeniser->context.current_comment.len == 0) {
			tokeniser->context.current_comment.data_off = pos;
			tokeniser->context.current_comment.len = len;
		} else {
			/* Need to do this to get length of '-' */
			len = pos -
				tokeniser->context.current_comment.data_off;
		}

		tokeniser->context.current_comment.len = len;

		tokeniser->state = HUBBUB_TOKENISER_STATE_COMMENT_END;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit comment */
		token.type = HUBBUB_TOKEN_COMMENT;
		token.data.comment = tokeniser->context.current_comment;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		if (tokeniser->context.current_comment.len == 0) {
			tokeniser->context.current_comment.data_off = pos;
		} else {
			/* Need to do this to get length of '--' */
			len += pos -
				tokeniser->context.current_comment.data_off;
		}

		tokeniser->context.current_comment.len = len;

		tokeniser->state = HUBBUB_TOKENISER_STATE_COMMENT;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_match_doctype(hubbub_tokeniser *tokeniser)
{
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (tokeniser->context.match_doctype.count == 1 &&
			(c & ~0x20) == 'O') {
		hubbub_inputstream_uppercase(tokeniser->input);
		tokeniser->context.match_doctype.count++;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (tokeniser->context.match_doctype.count == 2 &&
			(c & ~0x20) == 'C') {
		hubbub_inputstream_uppercase(tokeniser->input);
		tokeniser->context.match_doctype.count++;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (tokeniser->context.match_doctype.count == 3 &&
			(c & ~0x20) == 'T') {
		hubbub_inputstream_uppercase(tokeniser->input);
		tokeniser->context.match_doctype.count++;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (tokeniser->context.match_doctype.count == 4 &&
			(c & ~0x20) == 'Y') {
		hubbub_inputstream_uppercase(tokeniser->input);
		tokeniser->context.match_doctype.count++;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (tokeniser->context.match_doctype.count == 5 &&
			(c & ~0x20) == 'P') {
		hubbub_inputstream_uppercase(tokeniser->input);
		tokeniser->context.match_doctype.count++;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (tokeniser->context.match_doctype.count == 6 &&
			(c & ~0x20) == 'E') {
		hubbub_inputstream_uppercase(tokeniser->input);
		tokeniser->state = HUBBUB_TOKENISER_STATE_DOCTYPE;
		hubbub_inputstream_advance(tokeniser->input);
	} else {
		switch (tokeniser->context.match_doctype.count) {
		case 6: hubbub_inputstream_push_back(tokeniser->input, 'P');
		case 5: hubbub_inputstream_push_back(tokeniser->input, 'Y');
		case 4: hubbub_inputstream_push_back(tokeniser->input, 'T');
		case 3: hubbub_inputstream_push_back(tokeniser->input, 'C');
		case 2: hubbub_inputstream_push_back(tokeniser->input, 'O');
		case 1: hubbub_inputstream_push_back(tokeniser->input, 'D');
		}

		tokeniser->context.current_comment.data_off = 0;
		tokeniser->context.current_comment.len = 0;

		tokeniser->state = HUBBUB_TOKENISER_STATE_BOGUS_COMMENT;
	}

	return true;
}

bool hubbub_tokeniser_handle_doctype(hubbub_tokeniser *tokeniser)
{
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == ' ') {
		hubbub_inputstream_advance(tokeniser->input);
	}

	tokeniser->state = HUBBUB_TOKENISER_STATE_BEFORE_DOCTYPE_NAME;

	return true;
}

bool hubbub_tokeniser_handle_before_doctype_name(
		hubbub_tokeniser *tokeniser)
{
	hubbub_doctype *cdoc = &tokeniser->context.current_doctype;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == ' ') {
		hubbub_inputstream_advance(tokeniser->input);
	} else if ('a' <= c && c <= 'z') {
		uint32_t pos;
		size_t len;

		hubbub_inputstream_uppercase(tokeniser->input);

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		cdoc->name.data_off = pos;
		cdoc->name.len = len;
		cdoc->correct = false;

		tokeniser->state = HUBBUB_TOKENISER_STATE_DOCTYPE_NAME;

		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '>') {
		hubbub_token token;

		/* Emit doctype */
		token.type = HUBBUB_TOKEN_DOCTYPE;
		token.data.doctype = tokeniser->context.current_doctype;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit doctype */
		token.type = HUBBUB_TOKEN_DOCTYPE;
		token.data.doctype = tokeniser->context.current_doctype;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		cdoc->name.data_off = pos;
		cdoc->name.len = len;
		cdoc->correct = false;

		tokeniser->state = HUBBUB_TOKENISER_STATE_DOCTYPE_NAME;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_doctype_name(hubbub_tokeniser *tokeniser)
{
	hubbub_doctype *cdoc = &tokeniser->context.current_doctype;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == ' ') {
		tokeniser->state = HUBBUB_TOKENISER_STATE_AFTER_DOCTYPE_NAME;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '>') {
		hubbub_token token;

		/* Emit doctype */
		token.type = HUBBUB_TOKEN_DOCTYPE;
		token.data.doctype = tokeniser->context.current_doctype;
		token.data.doctype.correct =
			(hubbub_inputstream_compare_range_ascii(
				tokeniser->input,
				token.data.doctype.name.data_off,
				token.data.doctype.name.len,
				"HTML", SLEN("HTML")) == 0);

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if ('a' <= c && c <= 'z') {
		uint32_t pos;
		size_t len;

		hubbub_inputstream_uppercase(tokeniser->input);

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		cdoc->name.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit doctype */
		token.type = HUBBUB_TOKEN_DOCTYPE;
		token.data.doctype = tokeniser->context.current_doctype;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		uint32_t pos;
		size_t len;

		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		cdoc->name.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_after_doctype_name(hubbub_tokeniser *tokeniser)
{
	hubbub_doctype *cdoc = &tokeniser->context.current_doctype;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == ' ') {
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == '>') {
		hubbub_token token;

		/* Emit doctype */
		token.type = HUBBUB_TOKEN_DOCTYPE;
		token.data.doctype = tokeniser->context.current_doctype;
		token.data.doctype.correct =
			(hubbub_inputstream_compare_range_ascii(
				tokeniser->input,
				token.data.doctype.name.data_off,
				token.data.doctype.name.len,
				"HTML", SLEN("HTML")) == 0);

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit doctype */
		token.type = HUBBUB_TOKEN_DOCTYPE;
		token.data.doctype = tokeniser->context.current_doctype;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		cdoc->correct = false;

		tokeniser->state = HUBBUB_TOKENISER_STATE_BOGUS_DOCTYPE;

		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_handle_bogus_doctype(hubbub_tokeniser *tokeniser)
{
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '>') {
		hubbub_token token;

		/* Emit doctype */
		token.type = HUBBUB_TOKEN_DOCTYPE;
		token.data.doctype = tokeniser->context.current_doctype;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
		hubbub_inputstream_advance(tokeniser->input);
	} else if (c == HUBBUB_INPUTSTREAM_EOF) {
		hubbub_token token;

		/* Emit doctype */
		token.type = HUBBUB_TOKEN_DOCTYPE;
		token.data.doctype = tokeniser->context.current_doctype;

		hubbub_tokeniser_emit_token(tokeniser, &token);

		tokeniser->state = HUBBUB_TOKENISER_STATE_DATA;
	} else {
		hubbub_inputstream_advance(tokeniser->input);
	}

	return true;
}

bool hubbub_tokeniser_consume_entity(hubbub_tokeniser *tokeniser)
{
	uint32_t c;
	uint32_t pos;
	size_t len;

	if (tokeniser->context.match_entity.done_setup == false) {
		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		tokeniser->context.match_entity.str.data_off = pos;
		tokeniser->context.match_entity.str.len = len;
		tokeniser->context.match_entity.base = 0;
		tokeniser->context.match_entity.codepoint = 0;
		tokeniser->context.match_entity.had_data = false;
		tokeniser->context.match_entity.return_state =
				tokeniser->state;
		tokeniser->context.match_entity.complete = false;
		tokeniser->context.match_entity.done_setup = true;
		tokeniser->context.match_entity.context = NULL;
		tokeniser->context.match_entity.prev_len = len;

		hubbub_inputstream_advance(tokeniser->input);
	}

	c = hubbub_inputstream_peek(tokeniser->input);

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (c == '#') {
		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);

		tokeniser->context.match_entity.str.len += len;

		tokeniser->state = HUBBUB_TOKENISER_STATE_NUMBERED_ENTITY;
		hubbub_inputstream_advance(tokeniser->input);
	} else {
		tokeniser->state = HUBBUB_TOKENISER_STATE_NAMED_ENTITY;
	}

	return true;
}

bool hubbub_tokeniser_handle_numbered_entity(hubbub_tokeniser *tokeniser)
{
	hubbub_tokeniser_context *ctx = &tokeniser->context;
	uint32_t c = hubbub_inputstream_peek(tokeniser->input);
	uint32_t pos;
	size_t len;
	hubbub_error error;

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	if (ctx->match_entity.base == 0) {
		if ((c & ~0x20) == 'X') {
			ctx->match_entity.base = 16;

			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);
			ctx->match_entity.str.len += len;

			hubbub_inputstream_advance(tokeniser->input);
		} else {
			ctx->match_entity.base = 10;
		}
	}

	while ((c = hubbub_inputstream_peek(tokeniser->input)) !=
			HUBBUB_INPUTSTREAM_EOF &&
			c != HUBBUB_INPUTSTREAM_OOD) {
		if (ctx->match_entity.base == 10 &&
				('0' <= c && c <= '9')) {
			ctx->match_entity.had_data = true;

			ctx->match_entity.codepoint =
				ctx->match_entity.codepoint * 10 + (c - '0');

			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);
			ctx->match_entity.str.len += len;
		} else if (ctx->match_entity.base == 16 &&
				(('0' <= c && c <= '9') ||
				('A' <= (c & ~0x20) &&
						(c & ~0x20) <= 'F'))) {
			ctx->match_entity.had_data = true;

			ctx->match_entity.codepoint *= 16;

			if ('0' <= c && c <= '9') {
				ctx->match_entity.codepoint += (c - '0');
			} else {
				ctx->match_entity.codepoint +=
						((c & ~0x20) - 'A' + 10);
			}

			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);
			ctx->match_entity.str.len += len;
		} else {
			break;
		}

		hubbub_inputstream_advance(tokeniser->input);
	}

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	/* Eat trailing semicolon, if any */
	if (c == ';') {
		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);
		ctx->match_entity.str.len += len;

		hubbub_inputstream_advance(tokeniser->input);
	}

	/* Rewind the inputstream to start of matched sequence */
	hubbub_inputstream_rewind(tokeniser->input,
			ctx->match_entity.str.len);

	if (ctx->match_entity.had_data) {
		/* Had data, so calculate final codepoint */
		if (0x80 <= ctx->match_entity.codepoint &&
				ctx->match_entity.codepoint <= 0x9F) {
			ctx->match_entity.codepoint =
				cp1252Table[ctx->match_entity.codepoint -
						0x80];
		} else if (ctx->match_entity.codepoint == 0 ||
				ctx->match_entity.codepoint > 0x10FFFF) {
			ctx->match_entity.codepoint = 0xFFFD;
		}

		/* And replace the matched range with it */
		error = hubbub_inputstream_replace_range(tokeniser->input,
				ctx->match_entity.str.data_off,
				ctx->match_entity.str.len,
				ctx->match_entity.codepoint);
		if (error != HUBBUB_OK) {
			/** \todo handle memory exhaustion */
		}
	}

	/* Reset for next time */
	ctx->match_entity.done_setup = false;

	/* Flag completion */
	ctx->match_entity.complete = true;

	/* And back to the state we were entered in */
	tokeniser->state = ctx->match_entity.return_state;

	return true;
}

bool hubbub_tokeniser_handle_named_entity(hubbub_tokeniser *tokeniser)
{
	hubbub_tokeniser_context *ctx = &tokeniser->context;
	uint32_t c;
	uint32_t pos;
	size_t len;
	hubbub_error error;

	while ((c = hubbub_inputstream_peek(tokeniser->input)) !=
			HUBBUB_INPUTSTREAM_EOF &&
			c != HUBBUB_INPUTSTREAM_OOD) {
		uint32_t cp;

		if (c > 0x7F) {
			/* Entity names are ASCII only */
			break;
		}

		error = hubbub_entities_search_step((uint8_t) c,
				&cp,
				&ctx->match_entity.context);
		if (error == HUBBUB_OK) {
			/* Had a match - store it for later */
			ctx->match_entity.codepoint = cp;

			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);
			ctx->match_entity.str.len += len;

			/* And cache length, for replacement */
			ctx->match_entity.prev_len =
					ctx->match_entity.str.len;
		} else if (error == HUBBUB_INVALID) {
			/* No further matches - use last found */
			break;
		} else {
			pos = hubbub_inputstream_cur_pos(tokeniser->input,
					&len);
			ctx->match_entity.str.len += len;
		}

		hubbub_inputstream_advance(tokeniser->input);
	}

	if (c == HUBBUB_INPUTSTREAM_OOD)
		return false;

	/* Eat trailing semicolon, if any */
	if (ctx->match_entity.codepoint != 0 && c == ';' &&
			ctx->match_entity.prev_len ==
				ctx->match_entity.str.len) {
		pos = hubbub_inputstream_cur_pos(tokeniser->input, &len);
		ctx->match_entity.prev_len += len;
	}

	/* Rewind the inputstream to start of processed sequence */
	hubbub_inputstream_rewind(tokeniser->input,
			ctx->match_entity.str.len);

	/* Now, replace range, if we found a named entity */
	if (ctx->match_entity.codepoint != 0) {
		error = hubbub_inputstream_replace_range(tokeniser->input,
				ctx->match_entity.str.data_off,
				ctx->match_entity.prev_len,
				ctx->match_entity.codepoint);
		if (error != HUBBUB_OK) {
			/** \todo handle memory exhaustion */
		}
	}

	/* Reset for next time */
	ctx->match_entity.done_setup = false;

	/* Flag completion */
	ctx->match_entity.complete = true;

	/* And back to the state from whence we came */
	tokeniser->state = ctx->match_entity.return_state;

	return true;
}

/**
 * Handle input stream buffer moving
 *
 * \param buffer  Pointer to buffer
 * \param len     Length of data in buffer (bytes)
 * \param pw      Pointer to our context
 */
void hubbub_tokeniser_buffer_moved_handler(const uint8_t *buffer,
		size_t len, void *pw)
{
	hubbub_tokeniser *tok = (hubbub_tokeniser *) pw;

	tok->input_buffer = buffer;
	tok->input_buffer_len = len;

	if (tok->buffer_handler != NULL)
		tok->buffer_handler(buffer, len, tok->buffer_pw);
}

/**
 * Emit a token, performing sanity checks if necessary
 *
 * \param tokeniser  Tokeniser instance
 * \param token      Token to emit
 */
void hubbub_tokeniser_emit_token(hubbub_tokeniser *tokeniser,
		hubbub_token *token)
{
	if (tokeniser == NULL || token == NULL)
		return;

	/* Nothing to do if there's no registered handler */
	if (tokeniser->token_handler == NULL)
		return;

	if (token->type == HUBBUB_TOKEN_START_TAG ||
			token->type == HUBBUB_TOKEN_END_TAG) {
		uint32_t i, j;
		uint32_t n_attributes = token->data.tag.n_attributes;
		hubbub_attribute *attrs =
				token->data.tag.attributes;

		/* Discard duplicate attributes */
		for (i = 0; i < n_attributes; i++) {
			for (j = 0; j < n_attributes; j++) {
				uint32_t move;

				if (j == i ||
					attrs[i].name.len !=
							attrs[j].name.len ||
					hubbub_inputstream_compare_range_cs(
						tokeniser->input,
						attrs[i].name.data_off,
						attrs[j].name.data_off,
						attrs[i].name.len) != 0) {
					/* Attributes don't match */
					continue;
				}

				/* Calculate amount to move */
				move = (n_attributes - 1 -
					((i < j) ? j : i)) *
					sizeof(hubbub_attribute);

				if (move > 0) {
					memmove((i < j) ? &attrs[j]
							: &attrs[i],
						(i < j) ? &attrs[j+1]
							: &attrs[i+1],
						move);
				}

				/* And reduce the number of attributes */
				n_attributes--;
			}
		}

		token->data.tag.n_attributes = n_attributes;
	}

	/* Finally, emit token */
	tokeniser->token_handler(token, tokeniser->token_pw);
}
