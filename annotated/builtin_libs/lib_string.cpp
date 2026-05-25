#include "deegen_api.h"
#include "lualib_tonumber_util.h"
#include "runtime_utils.h"
#include "lj_strfmt.h"
#include <cstdio>
#include <cstdlib>

// This function works around static helper functions getting the linker error
// `error: undefined reference to 'DeegenImpl_ThrowErrorCString'`
// when they try and call ThrowError(), for some reason.
[[noreturn]] static void my_err(const char *msg)
{
    fprintf(stderr, "My error: %s\n", msg);
    exit(EXIT_FAILURE);
}

// string.byte -- https://www.lua.org/manual/5.1/manual.html#pdf-string.byte
//
// string.byte (s [, i [, j]])
// Returns the internal numerical codes of the characters s[i], s[i+1], ···, s[j]. The default value for i is 1; the default value for j is i.
//
// Note that numerical codes are not necessarily portable across platforms.
//
DEEGEN_DEFINE_LIB_FUNC(string_byte)
{
    size_t numArgs = GetNumArgs();
    if (unlikely(numArgs == 0))
    {
        ThrowError("bad argument #1 to 'byte' (string expected, got no value)");
    }

    GET_ARG_AS_STRING(byte, 1, ptr, ulen);

    int64_t lb;
    if (numArgs == 1)
    {
        lb = 1;
    }
    else
    {
        TValue tvLb = GetArg(1);
        auto [success, val] = LuaLib_ToNumber(tvLb);
        if (unlikely(!success))
        {
            ThrowError("bad argument #2 to 'byte' (number expected)");
        }
        lb = static_cast<int64_t>(val);
    }

    int64_t ub;
    if (numArgs < 3)
    {
        ub = lb;
    }
    else
    {
        TValue tvUb = GetArg(2);
        auto [success, val] = LuaLib_ToNumber(tvUb);
        if (unlikely(!success))
        {
            ThrowError("bad argument #3 to 'byte' (number expected)");
        }
        ub = static_cast<int64_t>(val);
    }

    int64_t len = static_cast<int64_t>(ulen);
    if (ub < 0) { ub += len + 1; }
    if (lb < 0) { lb += len + 1; }
    if (lb <= 0) { lb = 1; }
    if (ub > len) { ub = len; }

    if (unlikely(lb > ub))
    {
        Return();
    }

    TValue* sb = GetStackBase();
    ptr--;
    for (int64_t i = lb; i <= ub; i++)
    {
        uint8_t charVal = static_cast<uint8_t>(ptr[i]);
        sb[i - lb] = TValue::Create<tDouble>(charVal);
    }
    ReturnValueRange(sb, static_cast<size_t>(ub - lb + 1));
}

// Return -1 if not convertible to number, -2 if out of range
//
int64_t WARN_UNUSED NO_INLINE __attribute__((__preserve_most__)) TryConvertValueToStringCharNumericalCode(double tvDoubleView)
{
    TValue tv; tv.m_value = cxx2a_bit_cast<uint64_t>(tvDoubleView);
    if (!LuaLib_TVDoubleViewToNumberSlow(tvDoubleView /*inout*/))
    {
        return -1;
    }
    int64_t i64 = static_cast<int64_t>(tvDoubleView);
    if (i64 < 0 || i64 > 255)
    {
        return -2;
    }
    return i64;
}

// string.char -- https://www.lua.org/manual/5.1/manual.html#pdf-string.char
//
// string.char (···)
// Receives zero or more integers. Returns a string with length equal to the number of arguments, in which each character has the internal
// numerical code equal to its corresponding argument.
//
// Note that numerical codes are not necessarily portable across platforms.
//
DEEGEN_DEFINE_LIB_FUNC(string_char)
{
    size_t numArgs = GetNumArgs();
    TValue* sb = GetStackBase();
    SimpleTempStringStream ss;
    uint8_t* ptr = reinterpret_cast<uint8_t*>(ss.Reserve(numArgs + 1));
    for (size_t i = 0; i < numArgs; i++)
    {
        // If sb[i] is not a double, tvDoubleView will be NaN and i64 will never be within [0,255],
        // so we will go to slow path that does the full check, as desired.
        //
        double tvDoubleView = sb[i].ViewAsDouble();
        int64_t i64 = static_cast<int64_t>(tvDoubleView);
        if (unlikely(i64 < 0 || i64 > 255))
        {
            i64 = TryConvertValueToStringCharNumericalCode(tvDoubleView);
            if (unlikely(i64 < 0))
            {
                if (i64 == -1)
                {
                    ss.Destroy();
                    ThrowError("bad argument to 'char' (number expected)");
                }
                else
                {
                    assert(i64 == -2);
                    ss.Destroy();
                    ThrowError("bad argument to 'char' (invalid value)");
                }
            }
        }
        assert(0 <= i64 && i64 <= 255);
        ptr[i] = static_cast<uint8_t>(i64);
    }
    ptr[numArgs] = 0;

    HeapPtr<HeapString> res = VM::GetActiveVMForCurrentThread()->CreateStringObjectFromRawString(ptr, static_cast<uint32_t>(numArgs) /*len*/).As();
    ss.Destroy();
    Return(TValue::Create<tString>(res));
}

// string.dump -- https://www.lua.org/manual/5.1/manual.html#pdf-string.dump
//
// string.dump (function)
// Returns a string containing a binary representation of the given function, so that a later loadstring on this string returns a copy
// of the function. function must be a Lua function without upvalues.
//
DEEGEN_DEFINE_LIB_FUNC(string_dump)
{
    ThrowError("Library function 'string.dump' is not implemented yet!");
}

// ------------------------------------------------------------------------
// Pattern Matching Engine Core Infrastructure
// ------------------------------------------------------------------------

#define L_ESC '%'
#define CAP_UNFINISHED (-1)
#define CAP_POSITION (-2)
constexpr int LUA_MAXCAPTURES = 32;
constexpr int LJ_MAX_XLEVEL = 200;

struct MatchState {
    const char* src_init;
    const char* src_end;
    int level;
    int depth;
    struct {
        const char* init;
        ptrdiff_t len;
    } capture[LUA_MAXCAPTURES];
};

static bool match_class(int c, int cl)
{
    bool res;
    switch (std::tolower(cl))
    {
        case 'a': res = std::isalpha(c); break;
        case 'c': res = std::iscntrl(c); break;
        case 'd': res = std::isdigit(c); break;
        case 'g': res = std::isgraph(c); break;
        case 'l': res = std::islower(c); break;
        case 'p': res = std::ispunct(c); break;
        case 's': res = std::isspace(c); break;
        case 'u': res = std::isupper(c); break;
        case 'w': res = std::isalnum(c); break;
        case 'x': res = std::isxdigit(c); break;
        case 'z': res = (c == 0); break;
        default:  return (cl == c);
    }
    return std::isupper(cl) ? !res : res;
}

static const char* classend(const char* p)
{
    switch (*p++)
    {
        case L_ESC:
            if (unlikely(*p == '\0'))
            {
                my_err("malformed pattern (ends with '%')");
            }
            return p + 1;
        case '[':
            if (*p == '^') p++;
            do {
                if (unlikely(*p == '\0'))
                {
                    my_err("malformed pattern (missing ']')");
                }
                if (*(p++) == L_ESC && *p != '\0') p++;
            } while (*p != ']');
            return p + 1;
        default:
            return p;
    }
}

static bool matchbracketclass(int c, const char* p, const char* ec)
{
    bool sig = true;
    if (*(p + 1) == '^')
    {
        sig = false;
        p++;
    }
    while (++p < ec)
    {
        if (*p == L_ESC)
        {
            p++;
            if (match_class(c, static_cast<unsigned char>(*p))) return sig;
        }
        else if ((*(p + 1) == '-') && (p + 2 < ec))
        {
            p += 2;
            if (static_cast<unsigned char>(*(p - 2)) <= c && c <= static_cast<unsigned char>(*p)) return sig;
        }
        else if (static_cast<unsigned char>(*p) == c)
        {
            return sig;
        }
    }
    return !sig;
}

static bool singlematch(int c, const char* p, const char* ep)
{
    switch (*p)
    {
        case '.': return true;
        case L_ESC: return match_class(c, static_cast<unsigned char>(*(p + 1)));
        case '[': return matchbracketclass(c, p, ep - 1);
        default:  return (static_cast<unsigned char>(*p) == c);
    }
}

static const char* match(MatchState* ms, const char* s, const char* p);

static const char* matchbalance(MatchState* ms, const char* s, const char* p)
{
    if (unlikely(*p == 0 || *(p + 1) == 0))
    {
        my_err("malformed pattern (missing arguments to '%b')");
    }
    if (*s != *p) return nullptr;
    int b = *p;
    int e = *(p + 1);
    int cont = 1;
    while (++s < ms->src_end)
    {
        if (*s == e)
        {
            if (--cont == 0) return s + 1;
        }
        else if (*s == b)
        {
            cont++;
        }
    }
    return nullptr;
}

static const char* max_expand(MatchState* ms, const char* s, const char* p, const char* ep)
{
    ptrdiff_t i = 0;
    while ((s + i) < ms->src_end && singlematch(static_cast<unsigned char>(*(s + i)), p, ep))
    {
        i++;
    }
    while (i >= 0)
    {
        const char* res = match(ms, s + i, ep + 1);
        if (res) return res;
        i--;
    }
    return nullptr;
}

static const char* min_expand(MatchState* ms, const char* s, const char* p, const char* ep)
{
    for (;;)
    {
        const char* res = match(ms, s, ep + 1);
        if (res != nullptr) return res;
        if (s < ms->src_end && singlematch(static_cast<unsigned char>(*s), p, ep))
        {
            s++;
        }
        else
        {
            return nullptr;
        }
    }
}

static int check_capture(MatchState* ms, int l)
{
    l -= '1';
    if (unlikely(l < 0 || l >= ms->level || ms->capture[l].len == CAP_UNFINISHED))
    {
        my_err("invalid capture index");
    }
    return l;
}

static int capture_to_close(MatchState* ms)
{
    int level = ms->level;
    for (level--; level >= 0; level--)
    {
        if (ms->capture[level].len == CAP_UNFINISHED) return level;
    }
    my_err("invalid pattern capture to close");
}

static const char* start_capture(MatchState* ms, const char* s, const char* p, int what)
{
    int level = ms->level;
    if (unlikely(level >= LUA_MAXCAPTURES))
    {
        my_err("too many captures");
    }
    ms->capture[level].init = s;
    ms->capture[level].len = what;
    ms->level = level + 1;
    const char* res = match(ms, s, p);
    if (res == nullptr) ms->level--;
    return res;
}

static const char* end_capture(MatchState* ms, const char* s, const char* p)
{
    int l = capture_to_close(ms);
    ms->capture[l].len = s - ms->capture[l].init;
    const char* res = match(ms, s, p);
    if (res == nullptr) ms->capture[l].len = CAP_UNFINISHED;
    return res;
}

static const char* match_capture(MatchState* ms, const char* s, int l)
{
    l = check_capture(ms, l);
    size_t len = static_cast<size_t>(ms->capture[l].len);
    if (static_cast<size_t>(ms->src_end - s) >= len && memcmp(ms->capture[l].init, s, len) == 0)
    {
        return s + len;
    }
    return nullptr;
}

static const char* match(MatchState* ms, const char* s, const char* p)
{
    if (unlikely(++ms->depth > LJ_MAX_XLEVEL))
    {
        my_err("pattern matching nested too deeply");
    }
init:
    switch (*p)
    {
        case '(':
            if (*(p + 1) == ')') s = start_capture(ms, s, p + 2, CAP_POSITION);
            else s = start_capture(ms, s, p + 1, CAP_UNFINISHED);
            break;
        case ')':
            s = end_capture(ms, s, p + 1);
            break;
        case L_ESC:
            switch (*(p + 1))
            {
                case 'b':
                    s = matchbalance(ms, s, p + 2);
                    if (s == nullptr) break;
                    p += 4;
                    goto init;
                case 'f': {
                    p += 2;
                    if (unlikely(*p != '[')) my_err("missing '[' after '%f' in pattern");
                    const char* ep = classend(p);
                    char previous = (s == ms->src_init) ? '\0' : *(s - 1);
                    if (matchbracketclass(static_cast<unsigned char>(previous), p, ep - 1) ||
                        !matchbracketclass(static_cast<unsigned char>(*s), p, ep - 1))
                    {
                        s = nullptr;
                        break;
                    }
                    p = ep;
                    goto init;
                }
                default:
                    if (std::isdigit(static_cast<unsigned char>(*(p + 1))))
                    {
                        s = match_capture(ms, s, static_cast<unsigned char>(*(p + 1)));
                        if (s == nullptr) break;
                        p += 2;
                        goto init;
                    }
                    goto dflt;
            }
            break;
        case '\0':
            break;
        case '$':
            if (*(p + 1) != '\0') goto dflt;
            if (s != ms->src_end) s = nullptr;
            break;
        default: dflt: {
            const char* ep = classend(p);
            bool m = s < ms->src_end && singlematch(static_cast<unsigned char>(*s), p, ep);
            switch (*ep)
            {
                case '?': {
                    const char* res;
                    if (m && ((res = match(ms, s + 1, ep + 1)) != nullptr))
                    {
                        s = res;
                        break;
                    }
                    p = ep + 1;
                    goto init;
                }
                case '*':
                    s = max_expand(ms, s, p, ep);
                    break;
                case '+':
                    s = (m ? max_expand(ms, s + 1, p, ep) : nullptr);
                    break;
                case '-':
                    s = min_expand(ms, s, p, ep);
                    break;
                default:
                    if (m)
                    {
                        s++;
                        p = ep;
                        goto init;
                    }
                    s = nullptr;
                    break;
            }
            break;
        }
    }
    ms->depth--;
    return s;
}

// string.find -- https://www.lua.org/manual/5.1/manual.html#pdf-string.find
//
// string.find (s, pattern [, init [, plain]])
// Looks for the first match of pattern in the string s. If it finds a match, then find returns the indices of s where this occurrence
// starts and ends; otherwise, it returns nil. A third, optional numerical argument init specifies where to start the search; its default
// value is 1 and can be negative. A value of true as a fourth, optional argument plain turns off the pattern matching facilities, so the
// function does a plain "find substring" operation, with no characters in pattern being considered "magic". Note that if plain is given,
// then init must be given as well.
//
// If the pattern has captures, then in a successful match the captured values are also returned, after the two indices.
//
DEEGEN_DEFINE_LIB_FUNC(string_find)
{
    size_t numArgs = GetNumArgs();
    if (unlikely(numArgs < 2))
    {
        ThrowError("bad argument #2 to 'find' (string expected, got no value)");
    }

    GET_ARG_AS_STRING(find_s, 1, s, sLen);

    // Bypass the macro redefinition of `macro_argN2SBuf` safely inline
    #define macro_argN2SBuf macro_argN2SBuf_p
    GET_ARG_AS_STRING(find_p, 2, p, pLen);
    #undef macro_argN2SBuf

    int64_t initPos = 1;
    if (numArgs >= 3)
    {
        TValue tvInit = GetArg(2);
        auto [success, val] = LuaLib_ToNumber(tvInit);
        if (unlikely(!success))
        {
            ThrowError("bad argument #3 to 'find' (number expected)");
        }
        initPos = static_cast<int64_t>(val);
    }

    bool plain = false;
    if (numArgs >= 4)
    {
        // Treat any non-nil provided value as truthy
        TValue tvPlain = GetArg(3);
        plain = !tvPlain.IsNil();
    }

    int64_t len = static_cast<int64_t>(sLen);
    if (initPos < 0) { initPos += len + 1; }
    if (initPos <= 0) { initPos = 1; }
    if (initPos > len)
    {
        if (initPos > len + 1 || pLen > 0)
        {
            Return(TValue::Create<tNil>());
        }
    }

    if (plain)
    {
        if (pLen == 0)
        {
            TValue* sb = GetStackBase();
            sb[0] = TValue::Create<tDouble>(static_cast<double>(initPos));
            sb[1] = TValue::Create<tDouble>(static_cast<double>(initPos - 1));
            ReturnValueRange(sb, 2);
        }
        const char* haystack = s + initPos - 1;
        size_t haystackLen = sLen - static_cast<size_t>(initPos - 1);
        std::string_view hView(haystack, haystackLen);
        std::string_view pView(p, pLen);
        size_t pos = hView.find(pView);
        if (pos != std::string_view::npos)
        {
            int64_t startIdx = initPos + static_cast<int64_t>(pos);
            int64_t endIdx = startIdx + static_cast<int64_t>(pLen) - 1;
            TValue* sb = GetStackBase();
            sb[0] = TValue::Create<tDouble>(static_cast<double>(startIdx));
            sb[1] = TValue::Create<tDouble>(static_cast<double>(endIdx));
            ReturnValueRange(sb, 2);
        }
        Return(TValue::Create<tNil>());
    }

    const char* sstr = s + initPos - 1;
    const char* pstr = p;
    bool anchor = false;
    if (*pstr == '^')
    {
        pstr++;
        anchor = true;
    }

    MatchState ms;
    ms.src_init = s;
    ms.src_end = s + sLen;
    VM* vm = VM::GetActiveVMForCurrentThread();

    do
    {
        ms.level = 0;
        ms.depth = 0;
        const char* q = match(&ms, sstr, pstr);
        if (q != nullptr)
        {
            int64_t startIdx = (sstr - s) + 1;
            int64_t endIdx = q - s;
            TValue* sb = GetStackBase();
            sb[0] = TValue::Create<tDouble>(static_cast<double>(startIdx));
            sb[1] = TValue::Create<tDouble>(static_cast<double>(endIdx));

            for (int i = 0; i < ms.level; i++)
            {
                if (ms.capture[i].len == CAP_POSITION)
                {
                    sb[2 + i] = TValue::Create<tDouble>(static_cast<double>(ms.capture[i].init - ms.src_init + 1));
                }
                else
                {
                    HeapPtr<HeapString> res = vm->CreateStringObjectFromRawString(ms.capture[i].init, static_cast<uint32_t>(ms.capture[i].len)).As();
                    sb[2 + i] = TValue::Create<tString>(res);
                }
            }
            ReturnValueRange(sb, static_cast<size_t>(2 + ms.level));
        }
    } while (sstr++ <= ms.src_end && !anchor);

    Return(TValue::Create<tNil>());
}

// string.format -- https://www.lua.org/manual/5.1/manual.html#pdf-string.format
//
// string.format (formatstring, ···)
// Returns a formatted version of its variable number of arguments following the description given in its first argument (which must be
// a string). The format string follows the same rules as the printf family of standard C functions. The only differences are that the
// options/modifiers *, l, L, n, p, and h are not supported and that there is an extra option, q. The q option formats a string in a form
// suitable to be safely read back by the Lua interpreter: the string is written between double quotes, and all double quotes, newlines,
// embedded zeros, and backslashes in the string are correctly escaped when written. For instance, the call
//
//     string.format('%q', 'a string with "quotes" and \n new line')
// will produce the string:
//     "a string with \"quotes\" and \
//     new line"
//
// The options c, d, E, e, f, g, G, i, o, u, X, and x all expect a number as argument, whereas q and s expect a string.
//
// This function does not accept string values containing embedded zeros, except as arguments to the q option.
//
DEEGEN_DEFINE_LIB_FUNC(string_format)
{
    size_t numArgs = GetNumArgs();
    if (unlikely(numArgs == 0))
    {
        ThrowError("bad argument #1 to 'format' (string expected, got no value)");
    }

    VM* vm = VM::GetActiveVMForCurrentThread();
    TValue* sb = GetStackBase();
    GET_ARG_AS_STRING(format, 1, fmt, fmtLen);

    SimpleTempStringStream ss;
    StrFmtError resKind = StringFormatterWithLuaSemantics(&ss /*out*/, fmt, fmtLen, sb + 1 /*argBegin*/, numArgs - 1);
    if (likely(resKind == StrFmtNoError))
    {
        HeapPtr<HeapString> s = vm->CreateStringObjectFromRawString(ss.m_bufferBegin, static_cast<uint32_t>(ss.m_bufferCur - ss.m_bufferBegin)).As();
        ss.Destroy();
        Return(TValue::Create<tString>(s));
    }

    ss.Destroy();

    switch (resKind)
    {
    case StrFmtError_BadFmt:
        ThrowError("bad format string to 'format'");
    case StrFmtError_TooFewArgs:
        ThrowError("not enough arguments for format string");
    case StrFmtError_NotNumber:
        ThrowError("bad argument to 'format' (number expected)");
    case StrFmtError_NotString:
        ThrowError("bad argument to 'format' (string expected)");
    case StrFmtNoError:
        assert(false);
        __builtin_unreachable();
    }   /* switch resKind */
}

// string.gmatch -- https://www.lua.org/manual/5.1/manual.html#pdf-string.gmatch
//
// string.gmatch (s, pattern)
// Returns an iterator function that, each time it is called, returns the next captures from pattern over string s. If pattern specifies
// no captures, then the whole match is produced in each call.
//
// As an example, the following loop
//     s = "hello world from Lua"
//     for w in string.gmatch(s, "%a+") do
//         print(w)
//     end
// will iterate over all the words from string s, printing one per line.

// The next example collects all pairs key=value from the given string into a table:
//     t = {}
//     s = "from=world, to=Lua"
//     for k, v in string.gmatch(s, "(%w+)=(%w+)") do
//         t[k] = v
//     end
// For this function, a '^' at the start of a pattern does not work as an anchor, as this would prevent the iteration.
//
DEEGEN_DEFINE_LIB_FUNC(string_gmatch)
{
    ThrowError("Library function 'string.gmatch' is not implemented yet!");
}

// string.gsub -- https://www.lua.org/manual/5.1/manual.html#pdf-string.gsub
//
// string.gsub (s, pattern, repl [, n])
// Returns a copy of s in which all (or the first n, if given) occurrences of the pattern have been replaced by a replacement string
// specified by repl, which can be a string, a table, or a function. gsub also returns, as its second value, the total number of
// matches that occurred.
//
// If repl is a string, then its value is used for replacement. The character % works as an escape character: any sequence in repl of
// the form %n, with n between 1 and 9, stands for the value of the n-th captured substring (see below). The sequence %0 stands for
// the whole match. The sequence %% stands for a single %.
//
// If repl is a table, then the table is queried for every match, using the first capture as the key; if the pattern specifies no captures,
// then the whole match is used as the key.
//
// If repl is a function, then this function is called every time a match occurs, with all captured substrings passed as arguments,
// in order; if the pattern specifies no captures, then the whole match is passed as a sole argument.
//
// If the value returned by the table query or by the function call is a string or a number, then it is used as the replacement string;
// otherwise, if it is false or nil, then there is no replacement (that is, the original match is kept in the string).
//
// Here are some examples:
//
//     x = string.gsub("hello world", "(%w+)", "%1 %1")
//     --> x="hello hello world world"
//
//     x = string.gsub("hello world", "%w+", "%0 %0", 1)
//     --> x="hello hello world"
//
//     x = string.gsub("hello world from Lua", "(%w+)%s*(%w+)", "%2 %1")
//     --> x="world hello Lua from"
//
//     x = string.gsub("home = $HOME, user = $USER", "%$(%w+)", os.getenv)
//     --> x="home = /home/roberto, user = roberto"
//
//     x = string.gsub("4+5 = $return 4+5$", "%$(.-)%$", function (s)
//           return loadstring(s)()
//         end)
//     --> x="4+5 = 9"
//
//     local t = {name="lua", version="5.1"}
//     x = string.gsub("$name-$version.tar.gz", "%$(%w+)", t)
//     --> x="lua-5.1.tar.gz"
//
#include <cstring>
#include <cctype>

// Helper to append to SimpleTempStringStream safely
static void AppendToStream(SimpleTempStringStream& ss, const char* data, size_t len)
{
    if (len == 0) return;
    char* ptr = ss.Reserve(len);
    memcpy(ptr, data, len);
    ss.Update(ptr + len);
}

// FIX: Accept HeapPtr<HeapString> directly and index into it.
// This allows the compiler to handle the managed memory access.
static void add_gsub_repl_string(SimpleTempStringStream& ss, MatchState* ms, HeapPtr<HeapString> repl, const char* s, const char* match_end)
{
    uint32_t replLen = repl->m_length;
    for (uint32_t i = 0; i < replLen; i++)
    {
        // Accessing m_string[i] is safe because the compiler handles 
        // the address space translation for member access.
        uint8_t c = repl->m_string[i];

        if (c == '%' && i + 1 < replLen)
        {
            uint8_t next = repl->m_string[i + 1];
            i++; // consume the %
            
            if (isdigit(static_cast<unsigned char>(next)))
            {
                int capture_idx = next - '1';
                if (capture_idx == -1) // %0: full match
                {
                    AppendToStream(ss, s, static_cast<size_t>(match_end - s));
                }
                else if (capture_idx < ms->level && ms->capture[capture_idx].len != CAP_UNFINISHED)
                {
                    AppendToStream(ss, ms->capture[capture_idx].init, static_cast<size_t>(ms->capture[capture_idx].len));
                }
                else
                {
                    my_err("invalid capture index in replacement string");
                }
            }
            else if (next == '%')
            {
                AppendToStream(ss, "%", 1);
            }
            else
            {
                my_err("invalid use of '%' in replacement string");
            }
        }
        else
        {
            char ch = static_cast<char>(c);
            AppendToStream(ss, &ch, 1);
        }
    }
}

DEEGEN_DEFINE_LIB_FUNC(string_gsub)
{
    size_t numArgs = GetNumArgs();
    if (numArgs < 3)
    {
        ThrowError("bad arguments to 'gsub' (expected at least 3 arguments)");
    }

    GET_ARG_AS_STRING(gsub_s, 1, s, sLen);

    #define macro_argN2SBuf macro_argN2SBuf_p
    GET_ARG_AS_STRING(gsub_p, 2, p, pLen);
    (void)pLen;
    #undef macro_argN2SBuf

    TValue repl = GetArg(3);

    int64_t max_n = -1;
    if (numArgs >= 4)
    {
        TValue tvN = GetArg(4);
        auto [success, val] = LuaLib_ToNumber(tvN);
        if (unlikely(!success))
        {
            ThrowError("bad argument #4 to 'gsub' (number expected)");
        }
        max_n = static_cast<int64_t>(val);
    }

    VM* vm = VM::GetActiveVMForCurrentThread();
    SimpleTempStringStream ss;
    MatchState ms;
    ms.src_init = s;
    ms.src_end = s + sLen;

    const char* curr = s;
    int64_t n_matches = 0;

    while (max_n < 0 || n_matches < max_n)
    {
        ms.level = 0;
        ms.depth = 0;
        const char* e = match(&ms, curr, p);

        if (e != nullptr)
        {
            n_matches++;
            AppendToStream(ss, curr, static_cast<size_t>(e - curr));

            if (repl.Is<tString>())
            {
                HeapPtr<HeapString> hs = repl.As<tString>();
                // FIX: Simply pass the HeapPtr; the helper handles indexing
                add_gsub_repl_string(ss, &ms, hs, curr, e);
            }
            else if (repl.Is<tTable>())
            {
                ThrowError("gsub with table not yet implemented");
            }
            else if (repl.Is<tFunction>())
            {
                ThrowError("gsub with function not yet implemented");
            }
            else
            {
                 ThrowError("bad argument #3 to 'gsub' (string/function/table expected)");
            }

            if (e == curr)
            {
                if (curr < ms.src_end)
                {
                    AppendToStream(ss, curr, 1);
                    curr++;
                }
                else
                {
                    break;
                }
            }
            else
            {
                curr = e;
            }
        }
        else
        {
            break;
        }
    }

    if (curr < ms.src_end)
    {
        AppendToStream(ss, curr, static_cast<size_t>(ms.src_end - curr));
    }

    HeapPtr<HeapString> res = vm->CreateStringObjectFromRawString(ss.m_bufferBegin, static_cast<uint32_t>(ss.Len())).As();
    ss.Destroy();

    TValue* sb = GetStackBase();
    sb[0] = TValue::Create<tString>(res);
    sb[1] = TValue::Create<tDouble>(static_cast<double>(n_matches));
    ReturnValueRange(sb, 2);
}

// string.len -- https://www.lua.org/manual/5.1/manual.html#pdf-string.len
//
// string.len (s)
// Receives a string and returns its length. The empty string "" has length 0. Embedded zeros are counted, so "a\000bc\000" has length 5.
//
DEEGEN_DEFINE_LIB_FUNC(string_len)
{
    if (unlikely(GetNumArgs() == 0))
    {
        ThrowError("bad argument #1 to 'len' (string expected, got no value)");
    }
    GET_ARG_AS_STRING(len, 1, ptr, length);
    std::ignore = ptr;
    Return(TValue::Create<tDouble>(static_cast<double>(length)));
}

// Perform the simple 'toupper' or 'tolower' that just changes 'a-z' to 'A-Z' or vice versa.
// lb must be _mm_set1_epi8(0x60) for 'toupper' or _mm_set1_epi8(0x40) for 'tolower'
// ub must be _mm_set1_epi8(0x7b) for 'toupper' or _mm_set1_epi8(0x5b) for 'tolower'
// msk must be _mm_set1_epi8(0x20)
//
static __m128i ALWAYS_INLINE AlphabeticalToUpperOrLowerSimd(__m128i lb, __m128i ub, __m128i msk, __m128i input)
{
    __m128i x1 = _mm_cmpgt_epi8(input, lb);
    __m128i x2 = _mm_cmplt_epi8(input, ub);
    __m128i x3 = _mm_and_si128(x1, x2);
    __m128i x4 = _mm_and_si128(msk, x3);
    __m128i x5 = _mm_xor_si128(input, x4);
    return x5;
}

// The behavior of toupper/tolower is locale dependent so we cannot simply change 'a-z' to 'A-Z' (or vice versa)
// However, we can provide a fastpath for common locales where the rule is indeed changing 'a-z' to 'A-Z',
// and a fastpath for the shared common case where the char code is < 128 (in which the locale does not affect the behavior).
//
template<bool isToUpper>
static void ALWAYS_INLINE FastToUpperOrLower(const char* ptrIn, size_t length, char* ptrOut /*out*/)
{
    constexpr auto stdImplFn = isToUpper ? toupper : tolower;

    // Only attempt to use the optimized implementation if the string is at least 16 bytes, as we need to
    // check the locale, and initialize some SIMD registers...
    // This also handles the edge case, so our vectorized implementation can safely assume length >= 16
    //
    if (length >= 16)
    {
        // When 'nullptr' is passed in to 'setlocale', it only returns the current locale
        //
        const char* locale = std::setlocale(LC_CTYPE, nullptr /*queryLocale*/);

        // Check for the good case: locale is 'C' or 'en_US.UTF-8', where toupper/tolower has no bizzare behaviors.
        //
        bool isSimpleLocale = (locale[0] == 'C' && locale[1] == '\0') || strcmp(locale, "en_US.UTF-8") == 0;

        __m128i lb = _mm_set1_epi8(isToUpper ? 0x60 : 0x40);   // 'a'/'A' - 1
        __m128i ub = _mm_set1_epi8(isToUpper ? 0x7b : 0x5b);   // 'z'/'Z' + 1
        __m128i msk = _mm_set1_epi8(0x20);

        if (likely(isSimpleLocale))
        {
            // The rule is to simply change 'a-z' to 'A-Z'
            //
            const char* src = ptrIn;
            const char* end = ptrIn + length;
            char* dst = ptrOut;
            while (src + 16 <= end)
            {
                __m128i input = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
                __m128i res = AlphabeticalToUpperOrLowerSimd(lb, ub, msk, input);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), res);
                src += 16;
                dst += 16;
            }
            // This is correct since the string is at least 16 bytes
            //
            if (src < end)
            {
                __m128i input = _mm_loadu_si128(reinterpret_cast<const __m128i*>(end - 16));
                __m128i res = AlphabeticalToUpperOrLowerSimd(lb, ub, msk, input);
                dst = ptrOut + length - 16;
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), res);
            }
        }
        else
        {
            // We do not recognize the locale, but locale cannot change the toupper/tolower behavior for char code < 128.
            // So we check this and use fastpath if possible.
            //
            const char* src = ptrIn;
            const char* end = ptrIn + length;
            char* dst = ptrOut;
            while (src + 16 <= end)
            {
                __m128i input = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
                // Collect the MSB of each byte. If 'mask == 0', we know all characters are < 128
                //
                int mask = _mm_movemask_epi8(input);
                if (likely(mask == 0))
                {
                    __m128i res = AlphabeticalToUpperOrLowerSimd(lb, ub, msk, input);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), res);
                }
                else
                {
                    // Just call std toupper/tolower
                    //
                    for (size_t i = 0; i < 16; i++)
                    {
                        dst[i] = static_cast<char>(stdImplFn(static_cast<unsigned char>(src[i])));
                    }
                }
                src += 16;
                dst += 16;
            }
            // This is correct since the string is at least 16 bytes
            //
            if (src < end)
            {
                __m128i input = _mm_loadu_si128(reinterpret_cast<const __m128i*>(end - 16));
                // Collect the MSB of each byte. If 'mask == 0', we know all characters are < 128
                //
                int mask = _mm_movemask_epi8(input);
                if (likely(mask == 0))
                {
                    __m128i res = AlphabeticalToUpperOrLowerSimd(lb, ub, msk, input);
                    dst = ptrOut + length - 16;
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), res);
                }
                else
                {
                    while (src < end)
                    {
                        *dst = static_cast<char>(stdImplFn(static_cast<unsigned char>(*src)));
                        src++;
                        dst++;
                    }
                }
            }
        }
        // In debug mode, assert that our optimized implementation produced the same result as std toupper/tolower
        //
#ifndef NDEBUG
        for (size_t i = 0; i < length; i++)
        {
            assert(ptrOut[i] == static_cast<char>(stdImplFn(static_cast<unsigned char>(ptrIn[i]))));
        }
#endif
    }
    else
    {
        for (size_t i = 0; i < length; i++)
        {
            ptrOut[i] = static_cast<char>(stdImplFn(static_cast<unsigned char>(ptrIn[i])));
        }
    }
}

// string.lower -- https://www.lua.org/manual/5.1/manual.html#pdf-string.lower
//
// string.lower (s)
// Receives a string and returns a copy of this string with all uppercase letters changed to lowercase. All other characters are left
// unchanged. The definition of what an uppercase letter is depends on the current locale.
//
DEEGEN_DEFINE_LIB_FUNC(string_lower)
{
    size_t numArgs = GetNumArgs();
    if (unlikely(numArgs == 0))
    {
        ThrowError("bad argument #1 to 'lower' (string expected, got no value)");
    }

    GET_ARG_AS_STRING(lower, 1, ptr, length);

    SimpleTempStringStream ss;
    char* buf = ss.Reserve(length);

    FastToUpperOrLower<false /*isToUpper*/>(ptr /*in*/, length, buf /*out*/);

    VM* vm = VM::GetActiveVMForCurrentThread();
    HeapPtr<HeapString> res = vm->CreateStringObjectFromRawString(buf, static_cast<uint32_t>(length)).As();
    ss.Destroy();
    Return(TValue::Create<tString>(res));
}

// string.match -- https://www.lua.org/manual/5.1/manual.html#pdf-string.match
//
// string.match (s, pattern [, init])
// Looks for the first match of pattern in the string s. If it finds one, then match returns the captures from the pattern; otherwise
// it returns nil. If pattern specifies no captures, then the whole match is returned. A third, optional numerical argument init specifies
// where to start the search; its default value is 1 and can be negative.
//
DEEGEN_DEFINE_LIB_FUNC(string_match)
{
    size_t numArgs = GetNumArgs();
    if (unlikely(numArgs < 2))
    {
        ThrowError("bad argument #2 to 'match' (string expected, got no value)");
    }

    GET_ARG_AS_STRING(match_s, 1, s, sLen);

    // Bypass the macro redefinition of `macro_argN2SBuf` safely inline
    #define macro_argN2SBuf macro_argN2SBuf_p
    GET_ARG_AS_STRING(match_p, 2, p, pLen);
    (void)pLen;
    #undef macro_argN2SBuf

    int64_t initPos = 1;
    if (numArgs >= 3)
    {
        TValue tvInit = GetArg(2);
        auto [success, val] = LuaLib_ToNumber(tvInit);
        if (unlikely(!success))
        {
            ThrowError("bad argument #3 to 'match' (number expected)");
        }
        initPos = static_cast<int64_t>(val);
    }

    int64_t len = static_cast<int64_t>(sLen);
    if (initPos < 0) { initPos += len + 1; }
    if (initPos <= 0) { initPos = 1; }
    if (initPos > len + 1) { initPos = len + 1; }

    const char* sstr = s + initPos - 1;
    const char* pstr = p;
    bool anchor = false;
    if (*pstr == '^')
    {
        pstr++;
        anchor = true;
    }

    MatchState ms;
    ms.src_init = s;
    ms.src_end = s + sLen;
    VM* vm = VM::GetActiveVMForCurrentThread();

    do
    {
        ms.level = 0;
        ms.depth = 0;
        const char* q = match(&ms, sstr, pstr);
        if (q != nullptr)
        {
            if (ms.level == 0)
            {
                Return(TValue::Create<tString>(vm->CreateStringObjectFromRawString(sstr, static_cast<uint32_t>(q - sstr)).As()));
            }
            else
            {
                TValue* sb = GetStackBase();
                for (int i = 0; i < ms.level; i++)
                {
                    if (ms.capture[i].len == CAP_POSITION)
                    {
                        sb[i] = TValue::Create<tDouble>(static_cast<double>(ms.capture[i].init - ms.src_init + 1));
                    }
                    else
                    {
                        sb[i] = TValue::Create<tString>(vm->CreateStringObjectFromRawString(ms.capture[i].init, static_cast<uint32_t>(ms.capture[i].len)).As());
                    }
                }
                ReturnValueRange(sb, static_cast<size_t>(ms.level));
            }
        }
    } while (sstr++ < ms.src_end && !anchor);

    Return(TValue::Create<tNil>());
}

// string.rep -- https://www.lua.org/manual/5.1/manual.html#pdf-string.rep
//
// string.rep (s, n)
// Returns a string that is the concatenation of n copies of the string s.
//
DEEGEN_DEFINE_LIB_FUNC(string_rep)
{
    size_t numArgs = GetNumArgs();
    if (unlikely(numArgs < 2))
    {
        ThrowError("bad argument #2 to 'rep' (number expected, got no value)");
    }

    VM* vm = VM::GetActiveVMForCurrentThread();
    GET_ARG_AS_STRING(rep, 1, inputStr, inputStrLen);

    auto [success, numCopiesDbl] = LuaLib_ToNumber(GetArg(1));
    if (unlikely(!success))
    {
        ThrowError("bad argument #2 to 'rep' (number expected)");
    }

    int64_t numCopies = static_cast<int64_t>(numCopiesDbl);
    if (unlikely(numCopies <= 0))
    {
        Return(TValue::Create<tString>(vm->m_emptyString));
    }

    HeapPtr<HeapString> res = vm->CreateStringObjectFromConcatenationOfSameString(inputStr, static_cast<uint32_t>(inputStrLen), static_cast<size_t>(numCopies)).As();
    Return(TValue::Create<tString>(res));
}

// string.reverse -- https://www.lua.org/manual/5.1/manual.html#pdf-string.reverse
//
// string.reverse (s)
// Returns a string that is the string s reversed.
//
DEEGEN_DEFINE_LIB_FUNC(string_reverse)
{
    if (unlikely(GetNumArgs() == 0))
    {
        ThrowError("bad argument #1 to 'reverse' (string expected, got no value)");
    }
    GET_ARG_AS_STRING(reverse, 1, ptr, length);

    VM* vm = VM::GetActiveVMForCurrentThread();
    if (length == 0)
    {
        Return(TValue::Create<tString>(vm->m_emptyString));
    }

    SimpleTempStringStream ss;
    char* out = ss.Reserve(length);

    if (length <= 8)
    {
        // Since we always allocate 8-byte-aligned memory, the string starts at 8-byte-aligned address in HeapString,
        // and the string is non-empty (we checked length == 0 case above), 8 bytes following the string is always
        // dereferenceable even if string is less than 8 bytes long.
        //
        uint64_t originalVal = UnalignedLoad<uint64_t>(ptr);
        uint64_t reversedVal = __builtin_bswap64(originalVal);
        // The useful bytes are at the low bytes (due to little-endianness), and after bswap they go to the high bytes.
        // Shift them to low bytes so we can store it as the result.
        //
        reversedVal = reversedVal >> (64 - length * 8);
        UnalignedStore<uint64_t>(out, reversedVal);
    }
    else
    {
        // Byte swap and store all the 8-byte chunks
        //
        const char* src = ptr;
        const char* end = ptr + length;
        char* dst = out + length;
        while (src + 8 <= end)
        {
            uint64_t originalVal = UnalignedLoad<uint64_t>(src);
            uint64_t reversedVal = __builtin_bswap64(originalVal);
            dst -= 8;
            UnalignedStore<uint64_t>(dst, reversedVal);
            src += 8;
        }
        // For the remaining tail, we read 8 bytes from 'end - 8', reverse it and store to the beginning.
        // This is correct because length >= 8
        //
        if (src < end)
        {
            uint64_t originalVal = UnalignedLoad<uint64_t>(end - 8);
            uint64_t reversedVal = __builtin_bswap64(originalVal);
            UnalignedStore<uint64_t>(out, reversedVal);
        }
    }

    HeapPtr<HeapString> res = vm->CreateStringObjectFromRawString(out, static_cast<uint32_t>(length)).As();
    ss.Destroy();
    Return(TValue::Create<tString>(res));
}

// string.sub -- https://www.lua.org/manual/5.1/manual.html#pdf-string.sub
//
// string.sub (s, i [, j])
// Returns the substring of s that starts at i and continues until j; i and j can be negative. If j is absent, then it is assumed to be
// equal to -1 (which is the same as the string length). In particular, the call string.sub(s,1,j) returns a prefix of s with length j,
// and string.sub(s, -i) returns a suffix of s with length i.
//
DEEGEN_DEFINE_LIB_FUNC(string_sub)
{
    size_t numArgs = GetNumArgs();
    if (unlikely(numArgs == 0))
    {
        ThrowError("bad argument #1 to 'sub' (string expected, got no value)");
    }

    VM* vm = VM::GetActiveVMForCurrentThread();
    GET_ARG_AS_STRING(sub, 1, ptr, ulen);

    int64_t lb;
    if (numArgs == 1)
    {
        lb = 1;
    }
    else
    {
        TValue tvLb = GetArg(1);
        auto [success, val] = LuaLib_ToNumber(tvLb);
        if (unlikely(!success))
        {
            ThrowError("bad argument #2 to 'sub' (number expected)");
        }
        lb = static_cast<int64_t>(val);
    }

    int64_t ub;
    if (numArgs < 3)
    {
        ub = -1;
    }
    else
    {
        TValue tvUb = GetArg(2);
        auto [success, val] = LuaLib_ToNumber(tvUb);
        if (unlikely(!success))
        {
            ThrowError("bad argument #3 to 'sub' (number expected)");
        }
        ub = static_cast<int64_t>(val);
    }

    int64_t len = static_cast<int64_t>(ulen);
    if (ub < 0) { ub += len + 1; }
    if (lb < 0) { lb += len + 1; }
    if (lb <= 0) { lb = 1; }
    if (ub > len) { ub = len; }

    if (unlikely(lb > ub))
    {
        Return(TValue::Create<tString>(vm->m_emptyString));
    }
    else
    {
        Return(TValue::Create<tString>(vm->CreateStringObjectFromRawString(ptr + lb - 1, static_cast<uint32_t>(ub - lb + 1)).As()));
    }
}

// string.upper -- https://www.lua.org/manual/5.1/manual.html#pdf-string.upper
//
// string.upper (s)
// Receives a string and returns a copy of this string with all lowercase letters changed to uppercase. All other characters are left
// unchanged. The definition of what a lowercase letter is depends on the current locale.
//
DEEGEN_DEFINE_LIB_FUNC(string_upper)
{
    size_t numArgs = GetNumArgs();
    if (unlikely(numArgs == 0))
    {
        ThrowError("bad argument #1 to 'upper' (string expected, got no value)");
    }

    GET_ARG_AS_STRING(upper, 1, ptr, length);

    SimpleTempStringStream ss;
    char* buf = ss.Reserve(length);

    FastToUpperOrLower<true /*isToUpper*/>(ptr /*in*/, length, buf /*out*/);

    VM* vm = VM::GetActiveVMForCurrentThread();
    HeapPtr<HeapString> res = vm->CreateStringObjectFromRawString(buf, static_cast<uint32_t>(length)).As();
    ss.Destroy();
    Return(TValue::Create<tString>(res));
}

DEEGEN_END_LIB_FUNC_DEFINITIONS
