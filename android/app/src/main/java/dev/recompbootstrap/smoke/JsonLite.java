package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Minimal JSON (RFC 8259): objects, arrays, strings, numbers, true, false, null. Pure Java, no Android, no library.
 * Written for the app's own small files (settings.json, the setup lane's bundle.json and install.json).
 */
final class JsonLite {
    /** The JSON value null. Java null is never returned by parse and never accepted by write. */
    static final Object NULL = new Object() {
        @Override public String toString() { return "null"; }
    };
    /** Objects and arrays may be nested this deep (the outermost one counts as 1). */
    static final int MAX_DEPTH = 32;
    /** Longest accepted text, in characters (1 MiB). */
    static final int MAX_LENGTH = 1 << 20;

    static final class ParseException extends Exception {
        private static final long serialVersionUID = 1L;
        /** Index of the character where the problem was noticed. */
        final int position;
        ParseException(String message, int position) {
            super(message + " (at character " + position + ")");
            this.position = position;
        }
    }

    private final String text;
    private int at;

    private JsonLite(String text) { this.text = text; }

    /**
     * Returns LinkedHashMap&lt;String,Object&gt;, ArrayList&lt;Object&gt;, String, Long (a number written without fraction and
     * exponent that fits 64 bits), Double (every other number), Boolean or NULL.
     * Rejects: text after the value, duplicate keys in one object, nesting deeper than 32, text longer than 1 MiB.
     */
    static Object parse(String text) throws ParseException {
        if (text == null) throw new ParseException("no text", 0);
        if (text.length() > MAX_LENGTH) throw new ParseException("text longer than 1 MiB", MAX_LENGTH);
        JsonLite parser = new JsonLite(text);
        parser.blanks();
        Object value = parser.value(1);
        parser.blanks();
        if (parser.at != text.length()) throw new ParseException("text after the value", parser.at);
        return value;
    }

    /**
     * The inverse of parse for the same types (an Integer is also accepted and written as an integer). Map order is kept,
     * no spaces are written, and the result is plain ASCII: quotation mark, backslash, control characters and every
     * character above 0x7E are written as escapes.
     * IllegalArgumentException: any other type (including Java null), a key that is not a String, NaN or an infinity,
     * nesting deeper than 32.
     */
    static String write(Object value) {
        StringBuilder out = new StringBuilder();
        write(out, value, 1);
        return out.toString();
    }

    // ---- reading

    private void blanks() {
        while (at < text.length()) {
            char c = text.charAt(at);
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
            at++;
        }
    }

    private Object value(int depth) throws ParseException {
        if (at >= text.length()) throw new ParseException("a value is missing", at);
        char c = text.charAt(at);
        if (c == '{' || c == '[') {
            if (depth > MAX_DEPTH) throw new ParseException("nested deeper than " + MAX_DEPTH, at);
            return c == '{' ? object(depth) : array(depth);
        }
        if (c == '"') return string();
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        if (word("true")) return Boolean.TRUE;
        if (word("false")) return Boolean.FALSE;
        if (word("null")) return NULL;
        throw new ParseException("not a JSON value", at);
    }

    private boolean word(String word) {
        if (!text.startsWith(word, at)) return false;
        at += word.length();
        return true;
    }

    private Map<String, Object> object(int depth) throws ParseException {
        LinkedHashMap<String, Object> map = new LinkedHashMap<String, Object>();
        at++; // the brace
        blanks();
        if (at < text.length() && text.charAt(at) == '}') { at++; return map; }
        for (;;) {
            blanks();
            if (at >= text.length() || text.charAt(at) != '"') throw new ParseException("a key (a string) is expected", at);
            int keyAt = at;
            String key = string();
            if (map.containsKey(key)) throw new ParseException("the same key twice in one object", keyAt);
            blanks();
            if (at >= text.length() || text.charAt(at) != ':') throw new ParseException("':' is expected after a key", at);
            at++;
            blanks();
            map.put(key, value(depth + 1));
            blanks();
            if (at >= text.length()) throw new ParseException("the object is not closed", at);
            char c = text.charAt(at++);
            if (c == '}') return map;
            if (c != ',') throw new ParseException("',' or '}' is expected", at - 1);
        }
    }

    private List<Object> array(int depth) throws ParseException {
        ArrayList<Object> list = new ArrayList<Object>();
        at++; // the bracket
        blanks();
        if (at < text.length() && text.charAt(at) == ']') { at++; return list; }
        for (;;) {
            blanks();
            list.add(value(depth + 1));
            blanks();
            if (at >= text.length()) throw new ParseException("the array is not closed", at);
            char c = text.charAt(at++);
            if (c == ']') return list;
            if (c != ',') throw new ParseException("',' or ']' is expected", at - 1);
        }
    }

    private String string() throws ParseException {
        StringBuilder out = new StringBuilder();
        at++; // the opening quotation mark
        for (;;) {
            if (at >= text.length()) throw new ParseException("the string is not closed", at);
            char c = text.charAt(at++);
            if (c == '"') return out.toString();
            if (c < 0x20) throw new ParseException("a control character inside a string", at - 1);
            if (c != '\\') { out.append(c); continue; }
            if (at >= text.length()) throw new ParseException("the string is not closed", at);
            char escape = text.charAt(at++);
            switch (escape) {
                case '"': out.append('"'); break;
                case '\\': out.append('\\'); break;
                case '/': out.append('/'); break;
                case 'b': out.append('\b'); break;
                case 'f': out.append('\f'); break;
                case 'n': out.append('\n'); break;
                case 'r': out.append('\r'); break;
                case 't': out.append('\t'); break;
                case 'u': out.append(hex4()); break;
                default: throw new ParseException("unknown escape in a string", at - 1);
            }
        }
    }

    private char hex4() throws ParseException {
        if (at + 4 > text.length()) throw new ParseException("\\u needs four hexadecimal digits", at);
        int code = 0;
        for (int i = 0; i < 4; i++) {
            int digit = Character.digit(text.charAt(at), 16);
            // Character.digit also accepts digits of other scripts; JSON allows only 0-9, a-f, A-F
            if (digit < 0 || text.charAt(at) > 'f') throw new ParseException("\\u needs four hexadecimal digits", at);
            code = code * 16 + digit;
            at++;
        }
        return (char) code;
    }

    private boolean digitHere() {
        return at < text.length() && text.charAt(at) >= '0' && text.charAt(at) <= '9';
    }

    private Object number() throws ParseException {
        int start = at;
        if (text.charAt(at) == '-') at++;
        if (!digitHere()) throw new ParseException("a digit is expected", at);
        if (text.charAt(at) == '0') at++; // a leading zero stands alone: "01" is "0" followed by text
        else while (digitHere()) at++;
        boolean integer = true;
        if (at < text.length() && text.charAt(at) == '.') {
            integer = false;
            at++;
            if (!digitHere()) throw new ParseException("a digit is expected after '.'", at);
            while (digitHere()) at++;
        }
        if (at < text.length() && (text.charAt(at) == 'e' || text.charAt(at) == 'E')) {
            integer = false;
            at++;
            if (at < text.length() && (text.charAt(at) == '+' || text.charAt(at) == '-')) at++;
            if (!digitHere()) throw new ParseException("a digit is expected in the exponent", at);
            while (digitHere()) at++;
        }
        String digits = text.substring(start, at);
        if (integer) {
            try {
                return Long.valueOf(Long.parseLong(digits));
            } catch (NumberFormatException tooLargeForLong) {
                // falls through: kept as a Double
            }
        }
        double value = Double.parseDouble(digits);
        if (Double.isInfinite(value)) throw new ParseException("the number is out of range", start);
        return Double.valueOf(value);
    }

    // ---- writing

    private static void write(StringBuilder out, Object value, int depth) {
        if (value == NULL) {
            out.append("null");
        } else if (value instanceof String) {
            quote(out, (String) value);
        } else if (value instanceof Boolean) {
            out.append(((Boolean) value).booleanValue() ? "true" : "false");
        } else if (value instanceof Long || value instanceof Integer) {
            out.append(((Number) value).longValue());
        } else if (value instanceof Double) {
            double number = ((Double) value).doubleValue();
            if (Double.isNaN(number) || Double.isInfinite(number)) throw new IllegalArgumentException("JSON has no NaN and no infinity");
            out.append(Double.toString(number));
        } else if (value instanceof Map) {
            if (depth > MAX_DEPTH) throw new IllegalArgumentException("nested deeper than " + MAX_DEPTH);
            out.append('{');
            boolean first = true;
            for (Map.Entry<?, ?> entry : ((Map<?, ?>) value).entrySet()) {
                if (!(entry.getKey() instanceof String)) throw new IllegalArgumentException("a JSON key must be a String");
                if (!first) out.append(',');
                first = false;
                quote(out, (String) entry.getKey());
                out.append(':');
                write(out, entry.getValue(), depth + 1);
            }
            out.append('}');
        } else if (value instanceof List) {
            if (depth > MAX_DEPTH) throw new IllegalArgumentException("nested deeper than " + MAX_DEPTH);
            out.append('[');
            boolean first = true;
            for (Object element : (List<?>) value) {
                if (!first) out.append(',');
                first = false;
                write(out, element, depth + 1);
            }
            out.append(']');
        } else {
            throw new IllegalArgumentException("not a JSON value: " + (value == null ? "Java null" : value.getClass().getName()));
        }
    }

    private static void quote(StringBuilder out, String text) {
        out.append('"');
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\b': out.append("\\b"); break;
                case '\f': out.append("\\f"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:
                    if (c < 0x20 || c > 0x7E) {
                        out.append("\\u");
                        String hex = Integer.toHexString(c);
                        for (int pad = hex.length(); pad < 4; pad++) out.append('0');
                        out.append(hex);
                    } else {
                        out.append(c);
                    }
            }
        }
        out.append('"');
    }
}
