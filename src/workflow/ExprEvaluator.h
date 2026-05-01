#pragma once
// ExprEvaluator.h — GitHub Actions ${{ }} expression evaluator
//
// Implements a subset of the GitHub Actions expression language:
// https://docs.github.com/en/actions/learn-github-actions/expressions
//
// Supported:
//   - Literals: string ('text'), number (42, 3.14), bool (true/false), null
//   - Context access: github.ref, env.MY_VAR, steps.step_id.outputs.key
//   - Logical: &&, ||, !
//   - Comparison: ==, !=, <, <=, >, >=
//   - Functions: contains(), startsWith(), endsWith(), format(),
//                toJSON(), fromJSON(), join(), always(), success(),
//                failure(), cancelled(), hashFiles()
//   - Ternary-like via && || chaining
//
// NOT supported (yet):
//   - needs.job_id.result  (phase 5)
//   - jobs.job_id.*        (reusable workflows — not planned)
//   - matrix.*             (expanded before evaluation)

#include <string>
#include <map>
#include <variant>
#include <vector>
#include <functional>

namespace runner {

// ─── Value ────────────────────────────────────────────────────────────────

/// An expression value — can be null, bool, number, or string.
struct ExprValue {
    enum class Type { Null, Bool, Number, String };

    Type type = Type::Null;
    bool        b = false;
    double      n = 0.0;
    std::string s;

    // Constructors
    static ExprValue null()                     { return ExprValue{Type::Null, false, 0.0, ""}; }
    static ExprValue boolean(bool v)            { return ExprValue{Type::Bool, v, 0.0, ""}; }
    static ExprValue number(double v)           { return ExprValue{Type::Number, false, v, ""}; }
    static ExprValue string(std::string v)      { return ExprValue{Type::String, false, 0.0, std::move(v)}; }

    bool isTruthy() const;
    std::string toString() const;
    double toNumber() const;

    bool operator==(const ExprValue& o) const;
    bool operator!=(const ExprValue& o) const { return !(*this == o); }
};

// ─── Context ──────────────────────────────────────────────────────────────

/// The evaluation context provides named contexts (github, env, steps, etc.)
/// Each context is a recursive map of string → ExprValue.
class ExprContext {
public:
    /// Set a dotted-path value: set("github.ref", ExprValue::string("refs/heads/main"))
    void set(const std::string& path, ExprValue value);

    /// Retrieve a dotted-path value.
    ExprValue get(const std::string& path) const;

    /// Convenience: set a string value
    void setString(const std::string& path, const std::string& value) {
        set(path, ExprValue::string(value));
    }

    /// Expose the underlying flat map (for debugging)
    const std::map<std::string, ExprValue>& flat() const { return flat_; }

private:
    std::map<std::string, ExprValue> flat_;  // dotted paths → values
};

// ─── Evaluator ────────────────────────────────────────────────────────────

/// Evaluates GitHub Actions expressions.
class ExprEvaluator {
public:
    /// @param context  the expression context (github, env, steps, etc.)
    explicit ExprEvaluator(const ExprContext& context);

    /// Evaluate a complete ${{ expr }} string (including surrounding delimiters if present).
    /// If the input doesn't contain ${{, it's returned as-is.
    std::string evaluate(const std::string& input) const;

    /// Evaluate a bare expression (without ${{ }}) and return an ExprValue.
    ExprValue evaluateExpr(const std::string& expr) const;

    /// Evaluate a condition expression (returns bool).
    /// Used for step/job 'if:' conditions.
    bool evaluateCondition(const std::string& expr) const;

    /// Expand all ${{ }} occurrences in a string (text interpolation).
    std::string interpolate(const std::string& input) const;

    // ── Job status functions ─────────────────────────────────────────────
    // These must be configured before evaluating step 'if:' conditions.
    void setJobSuccess(bool v)    { job_success_    = v; }
    void setJobFailure(bool v)    { job_failure_    = v; }
    void setJobCancelled(bool v)  { job_cancelled_  = v; }

private:
    const ExprContext& ctx_;
    bool job_success_   = true;
    bool job_failure_   = false;
    bool job_cancelled_ = false;

    // ── Tokenizer ────────────────────────────────────────────────────────
    enum class TokenType {
        // Literals
        LitString, LitNumber, LitBool, LitNull,
        // Identifiers and dotted paths
        Identifier,
        // Operators
        OpAnd, OpOr, OpNot,
        OpEq, OpNeq, OpLt, OpLte, OpGt, OpGte,
        // Punctuation
        LParen, RParen, Comma, Dot,
        // End
        Eof
    };

    struct Token {
        TokenType   type;
        std::string text;
        double      num  = 0.0;
        bool        b    = false;
    };

    std::vector<Token> tokenize(const std::string& expr) const;

    // ── Recursive-descent parser ─────────────────────────────────────────
    struct Parser {
        const std::vector<Token>& tokens;
        size_t pos = 0;
        const ExprEvaluator& eval;

        const Token& peek() const  { return tokens[pos]; }
        const Token& consume()     { return tokens[pos++]; }
        bool atEnd() const         { return tokens[pos].type == TokenType::Eof; }

        ExprValue parseExpr();     // top-level
        ExprValue parseOr();
        ExprValue parseAnd();
        ExprValue parseNot();
        ExprValue parseComparison();
        ExprValue parsePrimary();
        ExprValue parseCall(const std::string& name);
    };

    // ── Built-in functions ───────────────────────────────────────────────
    ExprValue callFunction(const std::string& name,
                           const std::vector<ExprValue>& args) const;

    static ExprValue fnContains(const std::vector<ExprValue>& args);
    static ExprValue fnStartsWith(const std::vector<ExprValue>& args);
    static ExprValue fnEndsWith(const std::vector<ExprValue>& args);
    static ExprValue fnFormat(const std::vector<ExprValue>& args);
    static ExprValue fnJoin(const std::vector<ExprValue>& args);
    static ExprValue fnToJSON(const std::vector<ExprValue>& args);
    static ExprValue fnFromJSON(const std::vector<ExprValue>& args);
    ExprValue        fnAlways()    const { return ExprValue::boolean(true); }
    ExprValue        fnSuccess()   const { return ExprValue::boolean(job_success_); }
    ExprValue        fnFailure()   const { return ExprValue::boolean(job_failure_); }
    ExprValue        fnCancelled() const { return ExprValue::boolean(job_cancelled_); }
    ExprValue        fnHashFiles(const std::vector<ExprValue>& args) const;
};

} // namespace runner
