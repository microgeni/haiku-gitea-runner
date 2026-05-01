// ExprEvaluator.cpp — GitHub Actions expression language implementation
#include "ExprEvaluator.h"

#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <filesystem>
#include <fstream>
#include <set>
#include <glob.h>
#include <fnmatch.h>
#include <dirent.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>

namespace runner {

using json = nlohmann::json;

namespace fs = std::filesystem;

// ─── Recursive glob helper ───────────────────────────────────────────────
//
// Handles patterns containing "**" by walking the directory tree with
// opendir/readdir (POSIX, works on Haiku) and matching each file's path
// relative to the base directory against the pattern using fnmatch(3).
//
// Pattern semantics:
//   **/foo.txt  — matches foo.txt in any sub-directory (recursive)
//   src/**      — matches every file under src/
//   src/**/*.h  — matches all .h files under src/ recursively
//
// The function appends absolute paths of matching regular files to `results`.

static void globRecursive(const std::string& base_dir,
                           const std::string& pattern,
                           std::set<std::string>& results)
{
    // If the pattern has no "**", fall back to regular glob(3).
    if (pattern.find("**") == std::string::npos) {
        // Build absolute pattern
        std::string abs_pattern = (pattern[0] == '/') ? pattern
                                                       : (base_dir + "/" + pattern);
        glob_t gl;
        if (::glob(abs_pattern.c_str(), GLOB_NOSORT, nullptr, &gl) == 0) {
            for (size_t i = 0; i < gl.gl_pathc; ++i) {
                std::string path = gl.gl_pathv[i];
                try {
                    if (fs::is_regular_file(path)) results.insert(path);
                } catch (...) {}
            }
        }
        globfree(&gl);
        return;
    }

    // Pattern contains "**" — split into prefix (before **) and suffix (after).
    // We walk the directory tree matching each file's relative path.
    //
    // Normalise: ensure pattern is relative to base_dir.
    std::string rel_pattern = pattern;
    if (!rel_pattern.empty() && rel_pattern[0] == '/') {
        // Absolute pattern — strip the base_dir prefix if it's there
        if (rel_pattern.substr(0, base_dir.size()) == base_dir) {
            rel_pattern = rel_pattern.substr(base_dir.size());
            if (!rel_pattern.empty() && rel_pattern[0] == '/') rel_pattern = rel_pattern.substr(1);
        }
    }

    // Replace "**/" or "**" with a single-component wildcard for fnmatch
    // in each path component.  We use a simplified matcher: any file under
    // base_dir whose path relative to base_dir matches the pattern (with
    // FNM_PATHNAME disabled so ** can cross directory boundaries).

    // Walk the directory tree
    std::function<void(const std::string&)> walk = [&](const std::string& dir) {
        DIR* d = opendir(dir.c_str());
        if (!d) return;

        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;

            std::string abs_path = dir + "/" + name;

            // Use std::filesystem for portable type detection (Haiku's dirent
            // may not have d_type; filesystem::status() is always available).
            bool is_dir = false, is_reg = false;
            try {
                auto st = fs::status(abs_path);
                is_dir = fs::is_directory(st);
                is_reg = fs::is_regular_file(st);
            } catch (...) {
                continue;  // skip unreadable entries
            }

            if (is_reg) {
                // Compute path relative to base_dir
                std::string rel = abs_path.substr(base_dir.size());
                if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);

                // Match against the pattern (FNM_PATHNAME off so ** crosses dirs)
                if (fnmatch(rel_pattern.c_str(), rel.c_str(), 0) == 0) {
                    results.insert(abs_path);
                }
            }

            if (is_dir) {
                walk(abs_path);  // recurse
            }
        }
        closedir(d);
    };

    walk(base_dir);
}

// ═══════════════════════════════════════════════════════════════════════════
// ExprValue
// ═══════════════════════════════════════════════════════════════════════════

bool ExprValue::isTruthy() const {
    switch (type) {
        case Type::Null:   return false;
        case Type::Bool:   return b;
        case Type::Number: return n != 0.0 && !std::isnan(n);
        case Type::String: return !s.empty();
    }
    return false;
}

std::string ExprValue::toString() const {
    switch (type) {
        case Type::Null:   return "";
        case Type::Bool:   return b ? "true" : "false";
        case Type::Number: {
            // Mirror GitHub's number formatting (no trailing .0 for integers)
            if (n == std::floor(n) && std::abs(n) < 1e15) {
                return std::to_string(static_cast<int64_t>(n));
            }
            std::ostringstream oss;
            oss << n;
            return oss.str();
        }
        case Type::String: return s;
    }
    return "";
}

double ExprValue::toNumber() const {
    switch (type) {
        case Type::Null:   return 0.0;
        case Type::Bool:   return b ? 1.0 : 0.0;
        case Type::Number: return n;
        case Type::String: {
            try { return std::stod(s); } catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
        }
    }
    return 0.0;
}

bool ExprValue::operator==(const ExprValue& o) const {
    // GitHub loosely compares: coerce to common type
    if (type == Type::Null && o.type == Type::Null) return true;
    if (type == Type::Null || o.type == Type::Null) return false;
    if (type == Type::Bool || o.type == Type::Bool) return isTruthy() == o.isTruthy();
    if (type == Type::Number || o.type == Type::Number) return toNumber() == o.toNumber();
    // Both strings (case-insensitive per GitHub spec)
    std::string ls = s, rs = o.s;
    std::transform(ls.begin(), ls.end(), ls.begin(), ::tolower);
    std::transform(rs.begin(), rs.end(), rs.begin(), ::tolower);
    return ls == rs;
}

// ═══════════════════════════════════════════════════════════════════════════
// ExprContext
// ═══════════════════════════════════════════════════════════════════════════

void ExprContext::set(const std::string& path, ExprValue value) {
    flat_[path] = std::move(value);
}

ExprValue ExprContext::get(const std::string& path) const {
    // Direct lookup
    auto it = flat_.find(path);
    if (it != flat_.end()) return it->second;

    // Case-insensitive lookup
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (auto& [k, v] : flat_) {
        std::string kl = k;
        std::transform(kl.begin(), kl.end(), kl.begin(), ::tolower);
        if (kl == lower) return v;
    }

    return ExprValue::null();
}

// ═══════════════════════════════════════════════════════════════════════════
// ExprEvaluator — Tokenizer
// ═══════════════════════════════════════════════════════════════════════════

ExprEvaluator::ExprEvaluator(const ExprContext& context)
    : ctx_(context)
{}

static bool isIdentStart(char c) { return std::isalpha(c) || c == '_'; }
static bool isIdentChar(char c)  { return std::isalnum(c) || c == '_' || c == '-'; }

std::vector<ExprEvaluator::Token> ExprEvaluator::tokenize(const std::string& expr) const {
    std::vector<Token> tokens;
    size_t i = 0;
    const size_t len = expr.size();

    auto skipWS = [&]() {
        while (i < len && std::isspace(expr[i])) ++i;
    };

    while (i < len) {
        skipWS();
        if (i >= len) break;

        char c = expr[i];

        // String literal (single or double quoted)
        if (c == '\'' || c == '"') {
            char quote = c;
            ++i;
            std::string s;
            while (i < len && expr[i] != quote) {
                if (expr[i] == '\\' && i + 1 < len) { ++i; }
                s += expr[i++];
            }
            if (i < len) ++i; // consume closing quote
            tokens.push_back({TokenType::LitString, s});
            continue;
        }

        // Number literal
        if (std::isdigit(c) || (c == '-' && i+1 < len && std::isdigit(expr[i+1]))) {
            size_t start = i;
            if (c == '-') ++i;
            while (i < len && (std::isdigit(expr[i]) || expr[i] == '.')) ++i;
            std::string ns = expr.substr(start, i - start);
            tokens.push_back({TokenType::LitNumber, ns, std::stod(ns)});
            continue;
        }

        // Two-character operators
        if (i + 1 < len) {
            std::string two = expr.substr(i, 2);
            if (two == "&&") { tokens.push_back({TokenType::OpAnd, "&&"}); i += 2; continue; }
            if (two == "||") { tokens.push_back({TokenType::OpOr,  "||"}); i += 2; continue; }
            if (two == "==") { tokens.push_back({TokenType::OpEq,  "=="}); i += 2; continue; }
            if (two == "!=") { tokens.push_back({TokenType::OpNeq, "!="}); i += 2; continue; }
            if (two == "<=") { tokens.push_back({TokenType::OpLte, "<="}); i += 2; continue; }
            if (two == ">=") { tokens.push_back({TokenType::OpGte, ">="}); i += 2; continue; }
        }

        // Single-char operators / punctuation
        switch (c) {
            case '!': tokens.push_back({TokenType::OpNot, "!"}); ++i; continue;
            case '<': tokens.push_back({TokenType::OpLt,  "<"}); ++i; continue;
            case '>': tokens.push_back({TokenType::OpGt,  ">"}); ++i; continue;
            case '(': tokens.push_back({TokenType::LParen,"("}); ++i; continue;
            case ')': tokens.push_back({TokenType::RParen,")"}); ++i; continue;
            case ',': tokens.push_back({TokenType::Comma, ","}); ++i; continue;
            case '.': tokens.push_back({TokenType::Dot,   "."}); ++i; continue;
        }

        // Identifier or keyword
        if (isIdentStart(c)) {
            size_t start = i;
            while (i < len && isIdentChar(expr[i])) ++i;
            std::string id = expr.substr(start, i - start);

            if (id == "true")    { tokens.push_back({TokenType::LitBool, id, 0.0, true}); continue; }
            if (id == "false")   { tokens.push_back({TokenType::LitBool, id, 0.0, false}); continue; }
            if (id == "null")    { tokens.push_back({TokenType::LitNull, id}); continue; }

            tokens.push_back({TokenType::Identifier, id});
            continue;
        }

        // Unknown — skip
        ++i;
    }

    tokens.push_back({TokenType::Eof, ""});
    return tokens;
}

// ═══════════════════════════════════════════════════════════════════════════
// Parser
// ═══════════════════════════════════════════════════════════════════════════

ExprValue ExprEvaluator::Parser::parseExpr() { return parseOr(); }

ExprValue ExprEvaluator::Parser::parseOr() {
    auto left = parseAnd();
    while (peek().type == TokenType::OpOr) {
        consume();
        auto right = parseAnd();
        // || returns first truthy or last value
        left = left.isTruthy() ? left : right;
    }
    return left;
}

ExprValue ExprEvaluator::Parser::parseAnd() {
    auto left = parseNot();
    while (peek().type == TokenType::OpAnd) {
        consume();
        auto right = parseNot();
        // && returns last truthy or first falsy
        left = (!left.isTruthy()) ? left : right;
    }
    return left;
}

ExprValue ExprEvaluator::Parser::parseNot() {
    if (peek().type == TokenType::OpNot) {
        consume();
        auto v = parseNot();
        return ExprValue::boolean(!v.isTruthy());
    }
    return parseComparison();
}

ExprValue ExprEvaluator::Parser::parseComparison() {
    auto left = parsePrimary();

    auto tt = peek().type;
    if (tt == TokenType::OpEq  || tt == TokenType::OpNeq ||
        tt == TokenType::OpLt  || tt == TokenType::OpLte ||
        tt == TokenType::OpGt  || tt == TokenType::OpGte)
    {
        auto op = consume().type;
        auto right = parsePrimary();

        switch (op) {
            case TokenType::OpEq:  return ExprValue::boolean(left == right);
            case TokenType::OpNeq: return ExprValue::boolean(left != right);
            case TokenType::OpLt:  return ExprValue::boolean(left.toNumber() <  right.toNumber());
            case TokenType::OpLte: return ExprValue::boolean(left.toNumber() <= right.toNumber());
            case TokenType::OpGt:  return ExprValue::boolean(left.toNumber() >  right.toNumber());
            case TokenType::OpGte: return ExprValue::boolean(left.toNumber() >= right.toNumber());
            default: break;
        }
    }
    return left;
}

ExprValue ExprEvaluator::Parser::parsePrimary() {
    auto& tok = peek();

    // Parenthesised expression
    if (tok.type == TokenType::LParen) {
        consume();
        auto v = parseExpr();
        if (peek().type == TokenType::RParen) consume();
        return v;
    }

    // Literals
    if (tok.type == TokenType::LitString) { consume(); return ExprValue::string(tok.text); }
    if (tok.type == TokenType::LitNumber) { consume(); return ExprValue::number(tok.num); }
    if (tok.type == TokenType::LitBool)   { consume(); return ExprValue::boolean(tok.b); }
    if (tok.type == TokenType::LitNull)   { consume(); return ExprValue::null(); }

    // Identifier — context access or function call
    if (tok.type == TokenType::Identifier) {
        std::string name = tok.text;
        consume();

        // Dotted path: accumulate x.y.z
        while (peek().type == TokenType::Dot) {
            consume(); // consume '.'
            if (peek().type == TokenType::Identifier || peek().type == TokenType::LitNumber) {
                name += "." + consume().text;
            }
        }

        // Function call?
        if (peek().type == TokenType::LParen) {
            return parseCall(name);
        }

        // Context lookup
        return eval.ctx_.get(name);
    }

    return ExprValue::null();
}

ExprValue ExprEvaluator::Parser::parseCall(const std::string& name) {
    consume(); // consume '('
    std::vector<ExprValue> args;
    while (peek().type != TokenType::RParen && peek().type != TokenType::Eof) {
        args.push_back(parseExpr());
        if (peek().type == TokenType::Comma) consume();
    }
    if (peek().type == TokenType::RParen) consume();
    return eval.callFunction(name, args);
}

// ═══════════════════════════════════════════════════════════════════════════
// Built-in functions
// ═══════════════════════════════════════════════════════════════════════════

ExprValue ExprEvaluator::callFunction(const std::string& name,
                                       const std::vector<ExprValue>& args) const
{
    // Case-insensitive function name matching
    std::string lname = name;
    std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);

    if (lname == "contains")    return fnContains(args);
    if (lname == "startswith")  return fnStartsWith(args);
    if (lname == "endswith")    return fnEndsWith(args);
    if (lname == "format")      return fnFormat(args);
    if (lname == "join")        return fnJoin(args);
    if (lname == "tojson")      return fnToJSON(args);
    if (lname == "fromjson")    return fnFromJSON(args);
    if (lname == "always")      return fnAlways();
    if (lname == "success")     return fnSuccess();
    if (lname == "failure")     return fnFailure();
    if (lname == "cancelled")   return fnCancelled();
    if (lname == "hashfiles")   return fnHashFiles(args);

    return ExprValue::null();
}

// ─── hashFiles implementation ─────────────────────────────────────────────
//
// Spec (GitHub Actions):
//   hashFiles(pattern1, pattern2, ...) → lowercase hex SHA-256 string
//
// Algorithm:
//   1. For each glob pattern (relative to GITHUB_WORKSPACE), collect matching
//      regular files.  Deduplicate and sort lexicographically.
//   2. Compute SHA-256(contents) for each file in order.
//   3. Concatenate all 32-byte digests and compute a final SHA-256 of them.
//   4. Return the 64-character lowercase hex string.
//   5. If no files match any pattern, return "".
//
// Globs are resolved relative to GITHUB_WORKSPACE (from the expression
// context).  If GITHUB_WORKSPACE is not set, the current working directory
// is used as fallback.

ExprValue ExprEvaluator::fnHashFiles(const std::vector<ExprValue>& args) const
{
    if (args.empty()) return ExprValue::string("");

    // Determine workspace root
    std::string workspace;
    ExprValue ws_val = ctx_.get("env.GITHUB_WORKSPACE");
    if (ws_val.type == ExprValue::Type::String && !ws_val.s.empty()) {
        workspace = ws_val.s;
    } else {
        // Fallback: use GITHUB_WORKSPACE env var from process environment
        const char* env_ws = getenv("GITHUB_WORKSPACE");
        if (env_ws && *env_ws) workspace = env_ws;
    }
    // Final fallback: current directory
    if (workspace.empty()) workspace = ".";

    std::set<std::string> matched_set;  // deduplicates across patterns

    for (const auto& arg : args) {
        std::string pattern = arg.toString();
        if (pattern.empty()) continue;
        globRecursive(workspace, pattern, matched_set);
    }

    if (matched_set.empty()) return ExprValue::string("");

    // Sort lexicographically (set is already sorted)
    // Compute per-file SHA-256 and concatenate the 32-byte digests
    std::vector<uint8_t> all_digests;
    all_digests.reserve(matched_set.size() * 32);  // 32 bytes per SHA-256 digest

    for (const auto& path : matched_set) {
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;  // skip unreadable files

        EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
        if (!mdctx) continue;
        EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr);

        char buf[65536];
        while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
            EVP_DigestUpdate(mdctx,
                              reinterpret_cast<const uint8_t*>(buf),
                              static_cast<size_t>(f.gcount()));
        }

        uint8_t digest[EVP_MAX_MD_SIZE];
        unsigned int digest_len = 0;
        EVP_DigestFinal_ex(mdctx, digest, &digest_len);
        EVP_MD_CTX_free(mdctx);

        all_digests.insert(all_digests.end(), digest, digest + digest_len);
    }

    if (all_digests.empty()) return ExprValue::string("");

    // Final SHA-256 of all concatenated digests
    uint8_t final_digest[EVP_MAX_MD_SIZE];
    unsigned int final_len = 0;
    EVP_MD_CTX* final_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(final_ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(final_ctx, all_digests.data(), all_digests.size());
    EVP_DigestFinal_ex(final_ctx, final_digest, &final_len);
    EVP_MD_CTX_free(final_ctx);

    // Convert to lowercase hex string
    static const char hex[] = "0123456789abcdef";
    std::string result(final_len * 2, '0');
    for (unsigned int i = 0; i < final_len; ++i) {
        result[i * 2]     = hex[(final_digest[i] >> 4) & 0xf];
        result[i * 2 + 1] = hex[final_digest[i]        & 0xf];
    }
    return ExprValue::string(result);
}

ExprValue ExprEvaluator::fnContains(const std::vector<ExprValue>& args) {
    if (args.size() < 2) return ExprValue::boolean(false);
    std::string haystack = args[0].toString();
    std::string needle   = args[1].toString();
    // Case-insensitive
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
    std::transform(needle.begin(),   needle.end(),   needle.begin(),   ::tolower);
    return ExprValue::boolean(haystack.find(needle) != std::string::npos);
}

ExprValue ExprEvaluator::fnStartsWith(const std::vector<ExprValue>& args) {
    if (args.size() < 2) return ExprValue::boolean(false);
    std::string s = args[0].toString(), p = args[1].toString();
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);
    return ExprValue::boolean(s.substr(0, p.size()) == p);
}

ExprValue ExprEvaluator::fnEndsWith(const std::vector<ExprValue>& args) {
    if (args.size() < 2) return ExprValue::boolean(false);
    std::string s = args[0].toString(), p = args[1].toString();
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);
    if (p.size() > s.size()) return ExprValue::boolean(false);
    return ExprValue::boolean(s.substr(s.size() - p.size()) == p);
}

ExprValue ExprEvaluator::fnFormat(const std::vector<ExprValue>& args) {
    if (args.empty()) return ExprValue::string("");
    std::string fmt = args[0].toString();
    std::string result;
    size_t i = 0;
    while (i < fmt.size()) {
        if (fmt[i] == '{' && i+1 < fmt.size() && std::isdigit(fmt[i+1])) {
            size_t j = i + 1;
            while (j < fmt.size() && std::isdigit(fmt[j])) ++j;
            if (j < fmt.size() && fmt[j] == '}') {
                int idx = std::stoi(fmt.substr(i+1, j-i-1));
                if (idx + 1 < (int)args.size()) {
                    result += args[idx + 1].toString();
                }
                i = j + 1;
                continue;
            }
        }
        result += fmt[i++];
    }
    return ExprValue::string(result);
}

ExprValue ExprEvaluator::fnJoin(const std::vector<ExprValue>& args) {
    if (args.empty()) return ExprValue::string("");
    std::string sep = args.size() > 1 ? args[1].toString() : ",";
    // First arg should be array — we just stringify it
    return ExprValue::string(args[0].toString());
}

ExprValue ExprEvaluator::fnToJSON(const std::vector<ExprValue>& args) {
    if (args.empty()) return ExprValue::string("null");
    auto& v = args[0];
    switch (v.type) {
        case ExprValue::Type::Null:   return ExprValue::string("null");
        case ExprValue::Type::Bool:   return ExprValue::string(v.b ? "true" : "false");
        case ExprValue::Type::Number: return ExprValue::string(v.toString());
        case ExprValue::Type::String: {
            json j = v.s;
            return ExprValue::string(j.dump());
        }
    }
    return ExprValue::string("null");
}

ExprValue ExprEvaluator::fnFromJSON(const std::vector<ExprValue>& args) {
    if (args.empty()) return ExprValue::null();
    try {
        json j = json::parse(args[0].toString());
        if (j.is_string()) return ExprValue::string(j.get<std::string>());
        if (j.is_boolean()) return ExprValue::boolean(j.get<bool>());
        if (j.is_number()) return ExprValue::number(j.get<double>());
        return ExprValue::string(j.dump());
    } catch (...) {
        return ExprValue::null();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Public evaluation API
// ═══════════════════════════════════════════════════════════════════════════

ExprValue ExprEvaluator::evaluateExpr(const std::string& expr) const {
    // Trim whitespace
    std::string e = expr;
    while (!e.empty() && std::isspace(e.front())) e.erase(e.begin());
    while (!e.empty() && std::isspace(e.back()))  e.pop_back();

    if (e.empty()) return ExprValue::string("");

    auto tokens = tokenize(e);
    Parser parser{tokens, 0, *this};
    return parser.parseExpr();
}

bool ExprEvaluator::evaluateCondition(const std::string& expr) const {
    if (expr.empty()) return true;

    // Strip ${{ }} if present
    std::string e = expr;
    if (e.substr(0, 3) == "${{" && e.back() == '}') {
        e = e.substr(3, e.size() - 5);
    }

    return evaluateExpr(e).isTruthy();
}

std::string ExprEvaluator::interpolate(const std::string& input) const {
    std::string result;
    size_t i = 0;
    while (i < input.size()) {
        // Look for ${{
        if (i + 2 < input.size() && input[i] == '$' &&
            input[i+1] == '{' && input[i+2] == '{')
        {
            i += 3;
            // Find closing }}
            size_t start = i;
            int depth = 1;
            while (i < input.size()) {
                if (i + 1 < input.size() && input[i] == '}' && input[i+1] == '}') {
                    --depth;
                    if (depth == 0) break;
                }
                ++i;
            }
            std::string inner = input.substr(start, i - start);
            result += evaluateExpr(inner).toString();
            if (i + 1 < input.size()) i += 2; // skip }}
        } else {
            result += input[i++];
        }
    }
    return result;
}

std::string ExprEvaluator::evaluate(const std::string& input) const {
    // If the entire string is a ${{ }}, return evaluated value as string
    // Otherwise, interpolate all occurrences
    return interpolate(input);
}

} // namespace runner
