/*
 * This file is part of Hubbub.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2008 Andrew Sidwell <takkaria@netsurf-browser.org>
 */

#include <assert.h>
#include <string.h>

#include "treebuilder/modes.h"
#include "treebuilder/internal.h"
#include "treebuilder/treebuilder.h"
#include "utils/utils.h"


/**
 * Returns true iff there is an element in scope that has a namespace other
 * than the HTML namespace.
 */
static bool element_in_scope_in_non_html_ns(hubbub_treebuilder *treebuilder)
{
	element_context *stack = treebuilder->context.element_stack;
	uint32_t node;

	assert((signed) treebuilder->context.current_node >= 0);

	for (node = treebuilder->context.current_node; node > 0; node--) {
		element_type node_type = stack[node].type;

		/* The list of element types given in the spec here are the
		 * scoping elements excluding TABLE and HTML. TABLE is handled
		 * in the previous conditional and HTML should only occur
		 * as the first node in the stack, which is never processed
		 * in this loop. */
		if (node_type == TABLE || is_scoping_element(node_type))
			break;

		if (stack[node].ns != HUBBUB_NS_HTML)
			return true;
	}

	return false;
}

/**
 * Process a token as if in the secondary insertion mode.
 */
static void process_as_in_secondary(hubbub_treebuilder *treebuilder,
		const hubbub_token *token)
{
	/* Because we don't support calling insertion modes directly,
	 * instead we set the current mode to the secondary mode,
	 * call the token handler, and then reset the mode afterward
	 * as long as it's unchanged, as this has the same effect */

	treebuilder->context.mode = treebuilder->context.second_mode;

	hubbub_treebuilder_token_handler(token, treebuilder);

	if (treebuilder->context.mode == treebuilder->context.second_mode)
		treebuilder->context.mode = IN_FOREIGN_CONTENT;

	if (treebuilder->context.mode == IN_FOREIGN_CONTENT &&
			!element_in_scope_in_non_html_ns(treebuilder)) {
		treebuilder->context.mode =
				treebuilder->context.second_mode;
	}
}

/**
 * Break out of foreign content as a result of certain start tags or EOF.
 */
static void foreign_break_out(hubbub_treebuilder *treebuilder)
{
	element_context *stack = treebuilder->context.element_stack;

	/** \todo parse error */

	while (stack[treebuilder->context.current_node].ns !=
			HUBBUB_NS_HTML) {
		hubbub_ns ns;
		element_type type;
		void *node;

		element_stack_pop(treebuilder, &ns, &type, &node);

		treebuilder->tree_handler->unref_node(
				treebuilder->tree_handler->ctx,
				node);
	}

	treebuilder->context.mode = treebuilder->context.second_mode;
}

/**
 * Handle tokens in "in foreign content" insertion mode
 *
 * \param treebuilder  The treebuilder instance
 * \param token        The token to process
 * \return True to reprocess the token, false otherwise
 */
bool handle_in_foreign_content(hubbub_treebuilder *treebuilder,
		const hubbub_token *token)
{
	bool reprocess = false;

	switch (token->type) {
	case HUBBUB_TOKEN_CHARACTER:
		append_text(treebuilder, &token->data.character);
		break;
	case HUBBUB_TOKEN_COMMENT:
		process_comment_append(treebuilder, token,
				treebuilder->context.element_stack[
				treebuilder->context.current_node].node);
		break;
	case HUBBUB_TOKEN_DOCTYPE:
		/** \todo parse error */
		break;
	case HUBBUB_TOKEN_START_TAG:
	{
		hubbub_ns cur_node_ns = treebuilder->context.element_stack[
				treebuilder->context.current_node].ns;

		element_type cur_node = current_node(treebuilder);
		element_type type = element_type_from_name(treebuilder,
				&token->data.tag.name);

		if (cur_node_ns == HUBBUB_NS_HTML ||
				(cur_node_ns == HUBBUB_NS_MATHML &&
				(type != MGLYPH && type != MALIGNMARK) &&
				(cur_node == MI || cur_node == MO ||
				cur_node == MN || cur_node == MS ||
				cur_node == MTEXT))) {
			process_as_in_secondary(treebuilder, token);
		} else if (type == B || type ==  BIG || type == BLOCKQUOTE ||
				type == BODY || type == BR || type == CENTER ||
				type == CODE || type == DD || type == DIV ||
				type == DL || type == DT || type == EM ||
				type == EMBED || type == FONT || type == H1 ||
				type == H2 || type == H3 || type == H4 ||
				type == H5 || type == H6 || type == HEAD ||
				type == HR || type == I || type == IMG ||
				type == LI || type == LISTING ||
				type == MENU || type == META || type == NOBR ||
				type == OL || type == P || type == PRE ||
				type == RUBY || type == S || type == SMALL ||
				type == SPAN || type == STRONG ||
				type == STRIKE || type == SUB || type == SUP ||
				type == TABLE || type == TT || type == U ||
				type == UL || type == VAR) {
			foreign_break_out(treebuilder);
		} else {
			hubbub_tag tag = token->data.tag;

			adjust_foreign_attributes(treebuilder, &tag);

			/* Set to the right namespace and insert */
			tag.ns = cur_node_ns;

			if (token->data.tag.self_closing) {
				insert_element_no_push(treebuilder, &tag);
				/** \todo ack sc flag */
			} else {
				insert_element(treebuilder, &tag);
			}
		}
	}
		break;
	case HUBBUB_TOKEN_END_TAG:
		process_as_in_secondary(treebuilder, token);
		break;
	case HUBBUB_TOKEN_EOF:
		foreign_break_out(treebuilder);
		break;
	}

	return reprocess;
}
