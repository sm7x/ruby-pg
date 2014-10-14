/*
 * pg_text_encoder.c - PG::TextEncoder module
 * $Id$
 *
 */

/*
 *
 * Type casts for encoding Ruby objects to PostgreSQL string representations.
 *
 * Encoder classes are defined with pg_define_coder(). This creates a new coder class and
 * assigns an encoder function. The encoder function can decide between two different options
 * to return the encoded data. It can either return it as a Ruby String object or write the
 * encoded data to a memory space provided by the caller. In the second case, the encoder
 * function is called twice, once for deciding the encoding option and returning the expected
 * data length, and a second time when the requested memory space was made available by the
 * calling function, to do the actual conversion and writing. Parameter intermediate can be
 * used to store data between these two calls.
 *
 * Signature of all type cast encoders is:
 *    int encoder_function(t_pg_coder *this, VALUE value, char *out, VALUE *intermediate)
 *
 * Params:
 *   this  - The data part of the coder object that belongs to the encoder function.
 *   value - The Ruby object to cast.
 *   out   - NULL for the first call,
 *           pointer to a buffer with the requested size for the second call.
 *   intermediate - Pointer to a VALUE that might be set by the encoding function to some
 *           value in the first call that can be retrieved later in the second call.
 *           This VALUE is not yet initialized by the caller.
 *
 * Returns:
 *   >= 0  - If out==NULL the encoder function must return the expected output buffer size.
 *           This can be larger than the size of the second call, but may not be smaller.
 *           If out!=NULL the encoder function must return the actually used output buffer size
 *           without a termination character.
 *   -1    - The encoder function can alternatively return -1 to indicate that no second call
 *           is required, but the String value in *intermediate should be used instead.
 */


#include "pg.h"
#include "util.h"
#include <inttypes.h>

VALUE rb_mPG_TextEncoder;
static ID s_id_encode;
static ID s_id_to_i;
static VALUE hash_false_values;


VALUE
pg_obj_to_i( VALUE value )
{
	switch (TYPE(value)) {
		case T_FIXNUM:
		case T_FLOAT:
		case T_BIGNUM:
			return value;
		default:
			return rb_funcall(value, s_id_to_i, 0);
	}
}

/*
 * Document-class: PG::TextEncoder::Boolean < PG::SimpleEncoder
 *
 * This is the encoder class for the PostgreSQL bool type.
 *
 * Ruby values false, 0, '0', 'f', 'F', 'false', 'FALSE', 'off' and 'OFF'
 * are encoded as SQL +FALSE+ value. nil is sent as SQL +NULL+.
 * Any other values are encoded as SQL +TRUE+ .
 *
 */
static int
pg_text_enc_boolean(t_pg_coder *conv, VALUE value, char *out, VALUE *intermediate)
{
	if(out){
		switch( TYPE(value) ){
			case T_FALSE:
				*out = 'f';
				break;
			case T_FIXNUM:
			case T_BIGNUM:
				if( NUM2LONG(value) == 0 ){
					*out = 'f';
				} else {
					*out = 't';
				}
				break;
			case T_STRING:
				if( rb_hash_lookup(hash_false_values, value) == Qtrue ){
					*out = 'f';
					break;
				}
				/* fall through */
			default:
				*out = 't';
		}
	}
	return 1;
}


/*
 * Document-class: PG::TextEncoder::String < PG::SimpleEncoder
 *
 * This is the encoder class for the PostgreSQL text types.
 *
 * Non-String values are expected to have method +to_s+ defined.
 *
 */
int
pg_coder_enc_to_s(t_pg_coder *this, VALUE value, char *out, VALUE *intermediate)
{
	*intermediate = rb_obj_as_string(value);
	return -1;
}


/*
 * Document-class: PG::TextEncoder::Integer < PG::SimpleEncoder
 *
 * This is the encoder class for the PostgreSQL int types.
 *
 * Non-Integer values are expected to have method +to_i+ defined.
 *
 */
static int
pg_text_enc_integer(t_pg_coder *this, VALUE value, char *out, VALUE *intermediate)
{
	if(out){
		if(TYPE(*intermediate) == T_STRING){
			return pg_coder_enc_to_s(this, value, out, intermediate);
		}else{
			char *start = out;
			int len;
			int neg = 0;
			long long ll = NUM2LL(*intermediate);

			if (ll < 0) {
				/* We don't expect problems with the most negative integer not being representable
				 * as a positive integer, because Fixnum is only up to 63 bits.
				 */
				ll = -ll;
				neg = 1;
			}

			/* Compute the result string backwards. */
			do {
				long long remainder;
				long long oldval = ll;

				ll /= 10;
				remainder = oldval - ll * 10;
				*out++ = '0' + remainder;
			} while (ll != 0);

			if (neg)
				*out++ = '-';

			len = out - start;

			/* Reverse string. */
			out--;
			while (start < out)
			{
				char swap = *start;

				*start++ = *out;
				*out-- = swap;
			}

			return len;
		}
	}else{
		*intermediate = pg_obj_to_i(value);
		if(TYPE(*intermediate) == T_FIXNUM){
			int len;
			long long sll = NUM2LL(*intermediate);
			long long ll = sll < 0 ? -sll : sll;
			if( ll < 100000000 ){
				if( ll < 10000 ){
					if( ll < 100 ){
						len = ll < 10 ? 1 : 2;
					}else{
						len = ll < 1000 ? 3 : 4;
					}
				}else{
					if( ll < 1000000 ){
						len = ll < 100000 ? 5 : 6;
					}else{
						len = ll < 10000000 ? 7 : 8;
					}
				}
			}else{
				if( ll < 1000000000000LL ){
					if( ll < 10000000000LL ){
						len = ll < 1000000000LL ? 9 : 10;
					}else{
						len = ll < 100000000000LL ? 11 : 12;
					}
				}else{
					if( ll < 100000000000000LL ){
						len = ll < 10000000000000LL ? 13 : 14;
					}else{
						return pg_coder_enc_to_s(this, *intermediate, NULL, intermediate);
					}
				}
			}
			return sll < 0 ? len+1 : len;
		}else{
			return pg_coder_enc_to_s(this, *intermediate, NULL, intermediate);
		}
	}
}


/*
 * Document-class: PG::TextEncoder::Float < PG::SimpleEncoder
 *
 * This is the encoder class for the PostgreSQL float types.
 *
 */
static int
pg_text_enc_float(t_pg_coder *conv, VALUE value, char *out, VALUE *intermediate)
{
	if(out){
		double dvalue = NUM2DBL(value);
		/* Cast to the same strings as value.to_s . */
		if( isinf(dvalue) ){
			if( dvalue < 0 ){
				memcpy( out, "-Infinity", 9);
				return 9;
			} else {
				memcpy( out, "Infinity", 8);
				return 8;
			}
		} else if (isnan(dvalue)) {
			memcpy( out, "NaN", 3);
			return 3;
		}
		return sprintf( out, "%.16E", dvalue);
	}else{
		return 23;
	}
}

/*
 * Case-independent comparison of two not-necessarily-null-terminated strings.
 * At most n bytes will be examined from each string.
 */
static int
pg_strncasecmp(const char *s1, const char *s2, size_t n)
{
	while (n-- > 0)
	{
		unsigned char ch1 = (unsigned char) *s1++;
		unsigned char ch2 = (unsigned char) *s2++;

		if (ch1 != ch2){
			if (ch1 >= 'A' && ch1 <= 'Z')
				ch1 += 'a' - 'A';

			if (ch2 >= 'A' && ch2 <= 'Z')
				ch2 += 'a' - 'A';

			if (ch1 != ch2)
				return (int) ch1 - (int) ch2;
		}
		if (ch1 == 0)
			break;
	}
	return 0;
}

typedef int (*t_quote_func)( void *_this, char *p_in, int strlen, char *p_out );

static int
quote_array_buffer( void *_this, char *p_in, int strlen, char *p_out ){
	t_pg_composite_coder *this = _this;
	char *ptr1;
	char *ptr2;
	int backslashs = 0;
	int needquote;

	/* count data plus backslashes; detect chars needing quotes */
	if (strlen == 0)
		needquote = 1;   /* force quotes for empty string */
	else if (strlen == 4 && pg_strncasecmp(p_in, "NULL", strlen) == 0)
		needquote = 1;   /* force quotes for literal NULL */
	else
		needquote = 0;

	/* count required backlashs */
	for(ptr1 = p_in; ptr1 != p_in + strlen; ptr1++) {
		char ch = *ptr1;

		if (ch == '"' || ch == '\\'){
			needquote = 1;
			backslashs++;
		} else if (ch == '{' || ch == '}' || ch == this->delimiter ||
					ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f'){
			needquote = 1;
		}
	}

	if( needquote ){
		ptr1 = p_in + strlen;
		ptr2 = p_out + strlen + backslashs + 2;
		/* Write end quote */
		*--ptr2 = '"';

		/* Then store the escaped string on the final position, walking
			* right to left, until all backslashs are placed. */
		while( ptr1 != p_in ) {
			*--ptr2 = *--ptr1;
			if(*ptr2 == '"' || *ptr2 == '\\'){
				*--ptr2 = '\\';
			}
		}
		/* Write start quote */
		*p_out = '"';
		return strlen + backslashs + 2;
	} else {
		if( p_in != p_out )
			memcpy( p_out, p_in, strlen );
		return strlen;
	}
}

static char *
quote_string(t_pg_coder *this, VALUE value, VALUE string, char *current_out, int with_quote, t_quote_func quote_buffer, void *func_data)
{
	int strlen;
	VALUE subint;
	t_pg_coder_enc_func enc_func = pg_coder_enc_func(this);

	strlen = enc_func(this, value, NULL, &subint);

	if( strlen == -1 ){
		/* we can directly use String value in subint */
		strlen = RSTRING_LENINT(subint);

		if(with_quote){
			/* size of string assuming the worst case, that every character must be escaped. */
			current_out = pg_rb_str_ensure_capa( string, strlen * 2 + 2, current_out, NULL );

			current_out += quote_buffer( func_data, RSTRING_PTR(subint), strlen, current_out );
		} else {
			current_out = pg_rb_str_ensure_capa( string, strlen, current_out, NULL );
			memcpy( current_out, RSTRING_PTR(subint), strlen );
			current_out += strlen;
		}

	} else {

		if(with_quote){
			/* size of string assuming the worst case, that every character must be escaped
			 * plus two bytes for quotation.
			 */
			current_out = pg_rb_str_ensure_capa( string, 2 * strlen + 2, current_out, NULL );

			/* Place the unescaped string at current output position. */
			strlen = enc_func(this, value, current_out, &subint);

			current_out += quote_buffer( func_data, current_out, strlen, current_out );
		}else{
			/* size of the unquoted string */
			current_out = pg_rb_str_ensure_capa( string, strlen, current_out, NULL );
			current_out += enc_func(this, value, current_out, &subint);
		}
	}
	return current_out;
}

static char *
write_array(t_pg_composite_coder *this, VALUE value, char *current_out, VALUE string, int quote)
{
	int i;

	/* size of "{}" */
	current_out = pg_rb_str_ensure_capa( string, 2, current_out, NULL );
	*current_out++ = '{';

	for( i=0; i<RARRAY_LEN(value); i++){
		VALUE entry = rb_ary_entry(value, i);

		if( i > 0 ){
			current_out = pg_rb_str_ensure_capa( string, 1, current_out, NULL );
			*current_out++ = this->delimiter;
		}

		switch(TYPE(entry)){
			case T_ARRAY:
				current_out = write_array(this, entry, current_out, string, quote);
			break;
			case T_NIL:
				current_out = pg_rb_str_ensure_capa( string, 4, current_out, NULL );
				*current_out++ = 'N';
				*current_out++ = 'U';
				*current_out++ = 'L';
				*current_out++ = 'L';
				break;
			default:
				current_out = quote_string( this->elem, entry, string, current_out, quote, quote_array_buffer, this );
		}
	}
	current_out = pg_rb_str_ensure_capa( string, 1, current_out, NULL );
	*current_out++ = '}';
	return current_out;
}


/*
 * Document-class: PG::TextEncoder::Array < PG::CompositeEncoder
 *
 * This is the encoder class for PostgreSQL array types.
 *
 * All values are encoded according to the #elements_type
 * accessor. Sub-arrays are encoded recursively.
 *
 */
static int
pg_text_enc_array(t_pg_coder *conv, VALUE value, char *out, VALUE *intermediate)
{
	char *end_ptr;
	t_pg_composite_coder *this = (t_pg_composite_coder *)conv;
	*intermediate = rb_str_new(NULL, 0);

	end_ptr = write_array(this, value, RSTRING_PTR(*intermediate), *intermediate, this->needs_quotation);

	rb_str_set_len( *intermediate, end_ptr - RSTRING_PTR(*intermediate) );

	return -1;
}

static int
quote_identifier_buffer( void *_this, char *p_in, int strlen, char *p_out ){
	char *ptr1;
	char *ptr2;
	int backslashs = 0;

	/* count required backlashs */
	for(ptr1 = p_in; ptr1 != p_in + strlen; ptr1++) {
		if (*ptr1 == '"'){
			backslashs++;
		}
	}

	ptr1 = p_in + strlen;
	ptr2 = p_out + strlen + backslashs + 2;
	/* Write end quote */
	*--ptr2 = '"';

	/* Then store the escaped string on the final position, walking
		* right to left, until all backslashs are placed. */
	while( ptr1 != p_in ) {
		*--ptr2 = *--ptr1;
		if(*ptr2 == '"'){
			*--ptr2 = '"';
		}
	}
	/* Write start quote */
	*p_out = '"';
	return strlen + backslashs + 2;
}

static char *
pg_text_enc_array_identifier(t_pg_composite_coder *this, VALUE value, VALUE string, char *out)
{
	int i;
	int nr_elems;

	Check_Type(value, T_ARRAY);
	nr_elems = RARRAY_LEN(value);

	for( i=0; i<nr_elems; i++){
		VALUE entry = rb_ary_entry(value, i);

		out = quote_string(this->elem, entry, string, out, this->needs_quotation, quote_identifier_buffer, this);
		if( i < nr_elems-1 ){
			out = pg_rb_str_ensure_capa( string, 1, out, NULL );
			*out++ = '.';
		}
	}
	return out;
}

/*
 * Document-class: PG::TextEncoder::Identifier < PG::CompositeEncoder
 *
 * This is the encoder class for PostgreSQL identifiers.
 *
 * An Array value can be used for "schema.table.column" type identifiers:
 *   PG::TextEncoder::Identifier.new.encode(['schema', 'table', 'column'])
 *      => "schema"."table"."column"
 *
 */
static int
pg_text_enc_identifier(t_pg_coder *conv, VALUE value, char *out, VALUE *intermediate)
{
	t_pg_composite_coder *this = (t_pg_composite_coder *)conv;

	*intermediate = rb_str_new(NULL, 0);
	out = RSTRING_PTR(*intermediate);

	if( TYPE(value) == T_ARRAY){
		out = pg_text_enc_array_identifier(this, value, *intermediate, out);
	} else {
		out = quote_string(this->elem, value, *intermediate, out, this->needs_quotation, quote_identifier_buffer, this);
	}
	rb_str_set_len( *intermediate, out - RSTRING_PTR(*intermediate) );
	return -1;
}


static int
quote_literal_buffer( void *_this, char *p_in, int strlen, char *p_out ){
	char *ptr1;
	char *ptr2;
	int backslashs = 0;

	/* count required backlashs */
	for(ptr1 = p_in; ptr1 != p_in + strlen; ptr1++) {
		if (*ptr1 == '\''){
			backslashs++;
		}
	}

	ptr1 = p_in + strlen;
	ptr2 = p_out + strlen + backslashs + 2;
	/* Write end quote */
	*--ptr2 = '\'';

	/* Then store the escaped string on the final position, walking
		* right to left, until all backslashs are placed. */
	while( ptr1 != p_in ) {
		*--ptr2 = *--ptr1;
		if(*ptr2 == '\''){
			*--ptr2 = '\'';
		}
	}
	/* Write start quote */
	*p_out = '\'';
	return strlen + backslashs + 2;
}


/*
 * Document-class: PG::TextEncoder::QuotedLiteral < PG::CompositeEncoder
 *
 * This is the encoder class for PostgreSQL literals.
 *
 * A literal is quoted and escaped by the +'+ character.
 *
 */
static int
pg_text_enc_quoted_literal(t_pg_coder *conv, VALUE value, char *out, VALUE *intermediate)
{
	t_pg_composite_coder *this = (t_pg_composite_coder *)conv;

	*intermediate = rb_str_new(NULL, 0);
	out = RSTRING_PTR(*intermediate);
	out = quote_string(this->elem, value, *intermediate, out, this->needs_quotation, quote_literal_buffer, this);
	rb_str_set_len( *intermediate, out - RSTRING_PTR(*intermediate) );
	return -1;
}

/*
 * Document-class: PG::TextEncoder::ToBase64 < PG::CompositeEncoder
 *
 * This is an encoder class for conversion of binary to base64 data.
 *
 */
static int
pg_text_enc_to_base64(t_pg_coder *conv, VALUE value, char *out, VALUE *intermediate)
{
	int strlen;
	VALUE subint;
	t_pg_composite_coder *this = (t_pg_composite_coder *)conv;
	t_pg_coder_enc_func enc_func = pg_coder_enc_func(this->elem);

	if(out){
		/* Second encoder pass, if required */
		strlen = enc_func(this->elem, value, out, intermediate);
		base64_encode( out, out, strlen );

		return BASE64_ENCODED_SIZE(strlen);
	} else {
		/* First encoder pass */
		strlen = enc_func(this->elem, value, NULL, &subint);

		if( strlen == -1 ){
			/* Encoded string is returned in subint */
			VALUE out_str;

			strlen = RSTRING_LENINT(subint);
			out_str = rb_str_new(NULL, BASE64_ENCODED_SIZE(strlen));

			base64_encode( RSTRING_PTR(out_str), RSTRING_PTR(subint), strlen);
			*intermediate = out_str;

			return -1;
		} else {
			*intermediate = subint;

			return BASE64_ENCODED_SIZE(strlen);
		}
	}
}


void
init_pg_text_encoder()
{
	s_id_encode = rb_intern("encode");
	s_id_to_i = rb_intern("to_i");

	hash_false_values = rb_hash_new();
	rb_gc_register_address( &hash_false_values );
	rb_hash_aset( hash_false_values, rb_str_new2( "0" ), Qtrue );
	rb_hash_aset( hash_false_values, rb_str_new2( "f" ), Qtrue );
	rb_hash_aset( hash_false_values, rb_str_new2( "F" ), Qtrue );
	rb_hash_aset( hash_false_values, rb_str_new2( "false" ), Qtrue );
	rb_hash_aset( hash_false_values, rb_str_new2( "FALSE" ), Qtrue );
	rb_hash_aset( hash_false_values, rb_str_new2( "off" ), Qtrue );
	rb_hash_aset( hash_false_values, rb_str_new2( "OFF" ), Qtrue );

	/* This module encapsulates all encoder classes with text output format */
	rb_mPG_TextEncoder = rb_define_module_under( rb_mPG, "TextEncoder" );

	/* Make RDoc aware of the encoder classes... */
	/* dummy = rb_define_class_under( rb_mPG_TextEncoder, "Boolean", rb_cPG_SimpleEncoder ); */
	pg_define_coder( "Boolean", pg_text_enc_boolean, rb_cPG_SimpleEncoder, rb_mPG_TextEncoder );
	/* dummy = rb_define_class_under( rb_mPG_TextEncoder, "Integer", rb_cPG_SimpleEncoder ); */
	pg_define_coder( "Integer", pg_text_enc_integer, rb_cPG_SimpleEncoder, rb_mPG_TextEncoder );
	/* dummy = rb_define_class_under( rb_mPG_TextEncoder, "Float", rb_cPG_SimpleEncoder ); */
	pg_define_coder( "Float", pg_text_enc_float, rb_cPG_SimpleEncoder, rb_mPG_TextEncoder );
	/* dummy = rb_define_class_under( rb_mPG_TextEncoder, "String", rb_cPG_SimpleEncoder ); */
	pg_define_coder( "String", pg_coder_enc_to_s, rb_cPG_SimpleEncoder, rb_mPG_TextEncoder );

	/* dummy = rb_define_class_under( rb_mPG_TextEncoder, "Array", rb_cPG_CompositeEncoder ); */
	pg_define_coder( "Array", pg_text_enc_array, rb_cPG_CompositeEncoder, rb_mPG_TextEncoder );
	/* dummy = rb_define_class_under( rb_mPG_TextEncoder, "Identifier", rb_cPG_CompositeEncoder ); */
	pg_define_coder( "Identifier", pg_text_enc_identifier, rb_cPG_CompositeEncoder, rb_mPG_TextEncoder );
	/* dummy = rb_define_class_under( rb_mPG_TextEncoder, "QuotedLiteral", rb_cPG_CompositeEncoder ); */
	pg_define_coder( "QuotedLiteral", pg_text_enc_quoted_literal, rb_cPG_CompositeEncoder, rb_mPG_TextEncoder );
	/* dummy = rb_define_class_under( rb_mPG_TextEncoder, "ToBase64", rb_cPG_CompositeEncoder ); */
	pg_define_coder( "ToBase64", pg_text_enc_to_base64, rb_cPG_CompositeEncoder, rb_mPG_TextEncoder );
}