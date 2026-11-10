// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/profiles/object_query.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <system_error>
#include <utility>

#include <imos/codec/gen/views.h>

namespace imos::profiles {

namespace {

// ---- shared small helpers ----------------------------------------------

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string toLower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// Full-string numeric read; NaN when `text` is not entirely a number
/// (XPath number() of a non-numeric string).
double toNumber(std::string_view text) {
    // Trim XPath whitespace before conversion.
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    if (begin == end) {
        return std::nan("");
    }
    const std::string trimmed{text.substr(begin, end - begin)};
    char* stop = nullptr;
    const double value = std::strtod(trimmed.c_str(), &stop);
    if (stop != trimmed.c_str() + trimmed.size()) {
        return std::nan("");
    }
    return value;
}

std::optional<std::size_t> parseIndex(std::string_view text) {
    std::size_t value = 0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

// ---- subset-XPath tokenizer --------------------------------------------

struct Token {
    enum class Kind : std::uint8_t {
        Name,
        Number,
        String,
        Slash,
        LBracket,
        RBracket,
        LParen,
        RParen,
        Dot,
        Op,
        End,
    };
    Kind kind = Kind::End;
    std::string text;     ///< Name / String content
    double number = 0.0;  ///< Number value
    XPathOp op = XPathOp::Eq;
};

bool isNameStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.' ||
           c == ':';
}

Result<std::vector<Token>> tokenize(std::string_view text) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++i;
            continue;
        }
        Token token;
        switch (c) {
            case '/':
                token.kind = Token::Kind::Slash;
                ++i;
                break;
            case '[':
                token.kind = Token::Kind::LBracket;
                ++i;
                break;
            case ']':
                token.kind = Token::Kind::RBracket;
                ++i;
                break;
            case '(':
                token.kind = Token::Kind::LParen;
                ++i;
                break;
            case ')':
                token.kind = Token::Kind::RParen;
                ++i;
                break;
            case '=':
                token.kind = Token::Kind::Op;
                token.op = XPathOp::Eq;
                ++i;
                break;
            case '!':
                if (i + 1 >= text.size() || text[i + 1] != '=') {
                    return makeError(ErrorCode::ParseError, "XPath: '!' without '='");
                }
                token.kind = Token::Kind::Op;
                token.op = XPathOp::Ne;
                i += 2;
                break;
            case '<':
                token.kind = Token::Kind::Op;
                if (i + 1 < text.size() && text[i + 1] == '=') {
                    token.op = XPathOp::Le;
                    i += 2;
                } else {
                    token.op = XPathOp::Lt;
                    ++i;
                }
                break;
            case '>':
                token.kind = Token::Kind::Op;
                if (i + 1 < text.size() && text[i + 1] == '=') {
                    token.op = XPathOp::Ge;
                    i += 2;
                } else {
                    token.op = XPathOp::Gt;
                    ++i;
                }
                break;
            case '\'':
            case '"': {
                const char quote = c;
                const std::size_t close = text.find(quote, i + 1);
                if (close == std::string_view::npos) {
                    return makeError(ErrorCode::ParseError, "XPath: unterminated string literal");
                }
                token.kind = Token::Kind::String;
                token.text = std::string{text.substr(i + 1, close - i - 1)};
                i = close + 1;
                break;
            }
            case '.': {
                // '.5' is a number; a bare '.' is the context node.
                if (i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
                    char* stop = nullptr;
                    const std::string rest{text.substr(i)};
                    token.kind = Token::Kind::Number;
                    token.number = std::strtod(rest.c_str(), &stop);
                    i += static_cast<std::size_t>(stop - rest.c_str());
                } else {
                    token.kind = Token::Kind::Dot;
                    ++i;
                }
                break;
            }
            default: {
                if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
                    char* stop = nullptr;
                    const std::string rest{text.substr(i)};
                    token.kind = Token::Kind::Number;
                    token.number = std::strtod(rest.c_str(), &stop);
                    i += static_cast<std::size_t>(stop - rest.c_str());
                } else if (isNameStart(c)) {
                    std::size_t end = i + 1;
                    while (end < text.size() && isNameChar(text[end])) {
                        ++end;
                    }
                    token.kind = Token::Kind::Name;
                    token.text = std::string{text.substr(i, end - i)};
                    i = end;
                } else {
                    return makeError(ErrorCode::ParseError,
                                     std::string{"XPath: unexpected character '"} + c + "'");
                }
                break;
            }
        }
        tokens.push_back(std::move(token));
    }
    tokens.push_back(Token{});  // End sentinel
    return tokens;
}

// ---- subset-XPath parser -----------------------------------------------

class PredicateParser {
public:
    explicit PredicateParser(const std::vector<Token>& tokens, std::size_t start)
        : tokens_(tokens), pos_(start) {}

    Result<XPathExpr> parseOr() {
        Result<XPathExpr> first = parseAnd();
        if (!first.ok()) {
            return first;
        }
        XPathExpr expr = std::move(first).value();
        if (!peekKeyword("or")) {
            return expr;
        }
        XPathExpr orNode;
        orNode.kind = XPathExpr::Kind::Or;
        orNode.children.push_back(std::move(expr));
        while (peekKeyword("or")) {
            ++pos_;
            Result<XPathExpr> next = parseAnd();
            if (!next.ok()) {
                return next;
            }
            orNode.children.push_back(std::move(next).value());
        }
        return orNode;
    }

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] const Token& current() const { return tokens_[pos_]; }

private:
    Result<XPathExpr> parseAnd() {
        Result<XPathExpr> first = parseBool();
        if (!first.ok()) {
            return first;
        }
        XPathExpr expr = std::move(first).value();
        if (!peekKeyword("and")) {
            return expr;
        }
        XPathExpr andNode;
        andNode.kind = XPathExpr::Kind::And;
        andNode.children.push_back(std::move(expr));
        while (peekKeyword("and")) {
            ++pos_;
            Result<XPathExpr> next = parseBool();
            if (!next.ok()) {
                return next;
            }
            andNode.children.push_back(std::move(next).value());
        }
        return andNode;
    }

    Result<XPathExpr> parseBool() {
        const Token& token = tokens_[pos_];
        if (token.kind == Token::Kind::Name && tokens_[pos_ + 1].kind == Token::Kind::LParen) {
            if (equalsIgnoreCase(token.text, "not")) {
                pos_ += 2;
                Result<XPathExpr> inner = parseOr();
                if (!inner.ok()) {
                    return inner;
                }
                if (!expect(Token::Kind::RParen, "not(...)")) {
                    return makeError(ErrorCode::ParseError, "XPath: expected ')' after not(");
                }
                XPathExpr notNode;
                notNode.kind = XPathExpr::Kind::Not;
                notNode.children.push_back(std::move(inner).value());
                return notNode;
            }
            if (equalsIgnoreCase(token.text, "boolean")) {
                pos_ += 2;
                Result<XPathExpr> inner = parseOr();
                if (!inner.ok()) {
                    return inner;
                }
                if (!expect(Token::Kind::RParen, "boolean(...)")) {
                    return makeError(ErrorCode::ParseError, "XPath: expected ')' after boolean(");
                }
                return inner;  // boolean() of a boolean expression is itself
            }
            if (equalsIgnoreCase(token.text, "true") || equalsIgnoreCase(token.text, "false")) {
                const bool isTrue = equalsIgnoreCase(token.text, "true");
                pos_ += 2;
                if (!expect(Token::Kind::RParen, "true()/false()")) {
                    return makeError(ErrorCode::ParseError,
                                     "XPath: expected ')' in true()/false()");
                }
                XPathExpr leaf;
                leaf.kind = isTrue ? XPathExpr::Kind::True : XPathExpr::Kind::False;
                return leaf;
            }
        }
        return parseComparison();
    }

    Result<XPathExpr> parseComparison() {
        XPathExpr node;
        node.kind = XPathExpr::Kind::Compare;
        if (tokens_[pos_].kind == Token::Kind::Op) {
            // Implicit context left side: the spec's "[.>60 AND <120]".
            node.lhs = XPathOperand{};  // Context
        } else {
            Result<XPathOperand> lhs = parseOperand();
            if (!lhs.ok()) {
                return std::move(lhs).error();
            }
            node.lhs = std::move(lhs).value();
            if (tokens_[pos_].kind != Token::Kind::Op) {
                node.kind = XPathExpr::Kind::Truthy;
                node.rhs = node.lhs;
                return node;
            }
        }
        node.op = tokens_[pos_].op;
        ++pos_;
        Result<XPathOperand> rhs = parseOperand();
        if (!rhs.ok()) {
            return std::move(rhs).error();
        }
        node.rhs = std::move(rhs).value();
        return node;
    }

    Result<XPathOperand> parseOperand() {
        const Token& token = tokens_[pos_];
        XPathOperand operand;
        switch (token.kind) {
            case Token::Kind::Dot:
                operand.kind = XPathOperand::Kind::Context;
                ++pos_;
                return operand;
            case Token::Kind::Number:
                operand.kind = XPathOperand::Kind::Number;
                operand.number = token.number;
                ++pos_;
                return operand;
            case Token::Kind::String:
                operand.kind = XPathOperand::Kind::String;
                operand.text = token.text;
                ++pos_;
                return operand;
            case Token::Kind::Name: {
                const bool isString = equalsIgnoreCase(token.text, "string");
                const bool isNumber = equalsIgnoreCase(token.text, "number");
                if ((isString || isNumber) && tokens_[pos_ + 1].kind == Token::Kind::LParen) {
                    pos_ += 2;
                    Result<XPathOperand> inner = parseOperand();
                    if (!inner.ok()) {
                        return inner;
                    }
                    if (!expect(Token::Kind::RParen, "conversion")) {
                        return makeError(ErrorCode::ParseError,
                                         "XPath: expected ')' after conversion");
                    }
                    XPathOperand wrapped = std::move(inner).value();
                    wrapped.forceString = isString;
                    wrapped.forceNumber = isNumber;
                    return wrapped;
                }
                return makeError(ErrorCode::ParseError,
                                 "XPath: name '" + token.text +
                                     "' is not a subset operand (only '.', literals, and "
                                     "string()/number() conversions)");
            }
            default:
                return makeError(ErrorCode::ParseError, "XPath: expected an operand");
        }
    }

    bool peekKeyword(std::string_view word) const {
        const Token& token = tokens_[pos_];
        return token.kind == Token::Kind::Name && equalsIgnoreCase(token.text, word);
    }

    bool expect(Token::Kind kind, std::string_view /*where*/) {
        if (tokens_[pos_].kind != kind) {
            return false;
        }
        ++pos_;
        return true;
    }

    const std::vector<Token>& tokens_;
    std::size_t pos_;
};

// ---- subset-XPath evaluation -------------------------------------------

std::string operandString(const XPathOperand& operand, const std::string& contextValue) {
    switch (operand.kind) {
        case XPathOperand::Kind::Context:
            return contextValue;
        case XPathOperand::Kind::String:
            return operand.text;
        case XPathOperand::Kind::Number: {
            // Minimal XPath-style number-to-string (integers without decimals).
            // XPath string(): NaN, ±Infinity, integers without a decimal
            // point. The integer cast is only defined in range, so the
            // non-finite and out-of-range values are settled first.
            const double value = operand.number;
            if (std::isnan(value)) {
                return "NaN";
            }
            if (std::isinf(value)) {
                return value > 0 ? "Infinity" : "-Infinity";
            }
            if (std::fabs(value) < 1e15 &&
                value == static_cast<double>(static_cast<long long>(value))) {
                return std::to_string(static_cast<long long>(value));
            }
            return std::to_string(value);
        }
    }
    return {};
}

double operandNumber(const XPathOperand& operand, const std::string& contextValue) {
    if (operand.kind == XPathOperand::Kind::Number) {
        return operand.number;
    }
    return toNumber(operandString(operand, contextValue));
}

bool operandTruthy(const XPathOperand& operand, const std::string& contextValue) {
    if (operand.forceNumber || operand.kind == XPathOperand::Kind::Number) {
        const double value = operandNumber(operand, contextValue);
        return !std::isnan(value) && value != 0.0;
    }
    return !operandString(operand, contextValue).empty();
}

bool numericCompare(XPathOp op, double lhs, double rhs) {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return op == XPathOp::Ne;  // NaN != anything is the only true relation
    }
    switch (op) {
        case XPathOp::Eq:
            return lhs == rhs;
        case XPathOp::Ne:
            return lhs != rhs;
        case XPathOp::Lt:
            return lhs < rhs;
        case XPathOp::Gt:
            return lhs > rhs;
        case XPathOp::Le:
            return lhs <= rhs;
        case XPathOp::Ge:
            return lhs >= rhs;
    }
    return false;
}

bool stringCompare(XPathOp op, const std::string& lhs, const std::string& rhs) {
    const int cmp = lhs.compare(rhs);
    switch (op) {
        case XPathOp::Eq:
            return cmp == 0;
        case XPathOp::Ne:
            return cmp != 0;
        case XPathOp::Lt:
            return cmp < 0;
        case XPathOp::Gt:
            return cmp > 0;
        case XPathOp::Le:
            return cmp <= 0;
        case XPathOp::Ge:
            return cmp >= 0;
    }
    return false;
}

bool evaluateExpr(const XPathExpr& expr, const std::string& contextValue) {
    switch (expr.kind) {
        case XPathExpr::Kind::Or:
            return std::any_of(expr.children.begin(), expr.children.end(),
                               [&](const XPathExpr& c) { return evaluateExpr(c, contextValue); });
        case XPathExpr::Kind::And:
            return std::all_of(expr.children.begin(), expr.children.end(),
                               [&](const XPathExpr& c) { return evaluateExpr(c, contextValue); });
        case XPathExpr::Kind::Not:
            return !expr.children.empty() && !evaluateExpr(expr.children.front(), contextValue);
        case XPathExpr::Kind::True:
            return true;
        case XPathExpr::Kind::False:
            return false;
        case XPathExpr::Kind::Truthy:
            return operandTruthy(expr.rhs, contextValue);
        case XPathExpr::Kind::Compare: {
            const bool forcedString = expr.lhs.forceString || expr.rhs.forceString;
            const bool numeric = !forcedString && (expr.lhs.forceNumber || expr.rhs.forceNumber ||
                                                   expr.lhs.kind == XPathOperand::Kind::Number ||
                                                   expr.rhs.kind == XPathOperand::Kind::Number);
            if (numeric) {
                return numericCompare(expr.op, operandNumber(expr.lhs, contextValue),
                                      operandNumber(expr.rhs, contextValue));
            }
            return stringCompare(expr.op, operandString(expr.lhs, contextValue),
                                 operandString(expr.rhs, contextValue));
        }
    }
    return false;
}

void selectSteps(const codec::XmlNode& node, const std::vector<std::string>& steps,
                 std::size_t index, std::vector<const codec::XmlNode*>& out) {
    if (index == steps.size()) {
        out.push_back(&node);
        return;
    }
    for (const codec::XmlNode& child : node.children) {
        if (child.isElement(steps[index])) {
            selectSteps(child, steps, index + 1, out);
        }
    }
}

// ---- query evaluation helpers ------------------------------------------

/// The <mosPayload> roots eligible for field evaluation on one object:
/// every top-level <mosExternalMetadata> whose <mosSchema> matches the
/// query's (any block when the query names no schema).
std::vector<const codec::XmlNode*> payloadRoots(const codec::XmlNode& mosObj,
                                                const std::string& schema) {
    std::vector<const codec::XmlNode*> roots;
    for (const codec::XmlNode& child : mosObj.children) {
        if (!child.isElement("mosExternalMetadata")) {
            continue;
        }
        if (!schema.empty() && child.childText("mosSchema") != schema) {
            continue;
        }
        if (const codec::XmlNode* payload = child.firstChildElement("mosPayload")) {
            roots.push_back(payload);
        }
    }
    return roots;
}

bool fieldMatches(const ObjectQueryField& field, const std::vector<const codec::XmlNode*>& roots) {
    return std::any_of(roots.begin(), roots.end(), [&](const codec::XmlNode* root) {
        return evaluateField(field.expression, *root);
    });
}

/// The sort-key value for one object under one field: the string-value of
/// the first node the field's path selects, walking eligible payload roots
/// in document order ("" when nothing selects).
std::string sortValue(const FieldExpression& expression,
                      const std::vector<const codec::XmlNode*>& roots) {
    for (const codec::XmlNode* root : roots) {
        std::vector<const codec::XmlNode*> selected;
        selectSteps(*root, expression.steps, 0, selected);
        if (!selected.empty()) {
            return selected.front()->text();
        }
    }
    return {};
}

struct SortSpec {
    const ObjectQueryField* field = nullptr;
    int order = 0;
};

/// The deduplicated sort plan across every group, ordered by sortByOrder
/// (spec rule: same field name ⇒ same attributes in every group).
std::vector<SortSpec> sortPlan(const ObjectQuery& query) {
    std::vector<SortSpec> plan;
    for (const ObjectQueryGroup& group : query.groups) {
        for (const ObjectQueryField& field : group.fields) {
            if (field.sortByOrder < 0) {
                continue;
            }
            const bool seen = std::any_of(plan.begin(), plan.end(), [&](const SortSpec& spec) {
                return spec.field->expression.sourceText == field.expression.sourceText;
            });
            if (!seen) {
                plan.push_back(SortSpec{&field, field.sortByOrder});
            }
        }
    }
    std::stable_sort(plan.begin(), plan.end(),
                     [](const SortSpec& a, const SortSpec& b) { return a.order < b.order; });
    return plan;
}

}  // namespace

// ---- public: subset XPath ----------------------------------------------

Result<FieldExpression> parseFieldXPath(std::string_view xpath) {
    Result<std::vector<Token>> tokenized = tokenize(xpath);
    if (!tokenized.ok()) {
        return std::move(tokenized).error();
    }
    const std::vector<Token>& tokens = tokenized.value();

    FieldExpression field;
    field.sourceText = std::string{xpath};

    std::size_t pos = 0;
    if (tokens[pos].kind == Token::Kind::Slash) {
        ++pos;  // the leading '/' is optional and does not change the anchor
    }
    while (tokens[pos].kind == Token::Kind::Name) {
        field.steps.push_back(tokens[pos].text);
        ++pos;
        if (tokens[pos].kind != Token::Kind::Slash) {
            break;
        }
        ++pos;
    }
    if (field.steps.empty()) {
        return makeError(ErrorCode::ParseError, "XPath: expected a location path");
    }

    if (tokens[pos].kind == Token::Kind::LBracket) {
        PredicateParser parser{tokens, pos + 1};
        Result<XPathExpr> predicate = parser.parseOr();
        if (!predicate.ok()) {
            return std::move(predicate).error();
        }
        pos = parser.position();
        if (tokens[pos].kind != Token::Kind::RBracket) {
            return makeError(ErrorCode::ParseError, "XPath: expected ']' closing the predicate");
        }
        ++pos;
        field.predicate = std::move(predicate).value();
    }
    if (tokens[pos].kind != Token::Kind::End) {
        return makeError(ErrorCode::ParseError, "XPath: trailing content after the expression");
    }
    return field;
}

bool evaluateField(const FieldExpression& field, const codec::XmlNode& context) {
    std::vector<const codec::XmlNode*> selected;
    selectSteps(context, field.steps, 0, selected);
    if (!field.predicate.has_value()) {
        return !selected.empty();  // existence
    }
    return std::any_of(selected.begin(), selected.end(), [&](const codec::XmlNode* node) {
        const std::string value = node->text();
        return evaluateExpr(*field.predicate, value);
    });
}

// ---- public: generalSearch ---------------------------------------------

bool evaluateGeneralSearch(std::string_view expression, std::string_view corpus) {
    struct Term {
        std::string needle;  ///< lowercase
        bool negated = false;
    };
    std::vector<std::vector<Term>> groups(1);  // OR of AND-groups
    bool negateNext = false;

    std::size_t i = 0;
    while (i < expression.size()) {
        while (i < expression.size() &&
               std::isspace(static_cast<unsigned char>(expression[i])) != 0) {
            ++i;
        }
        std::size_t end = i;
        while (end < expression.size() &&
               std::isspace(static_cast<unsigned char>(expression[end])) == 0) {
            ++end;
        }
        if (end == i) {
            break;
        }
        const std::string word = toLower(expression.substr(i, end - i));
        i = end;
        if (word == "and") {
            continue;  // adjacency already conjoins
        }
        if (word == "or") {
            groups.emplace_back();
            negateNext = false;
            continue;
        }
        if (word == "not") {
            negateNext = true;
            continue;
        }
        groups.back().push_back(Term{word, negateNext});
        negateNext = false;
    }

    const std::string haystack = toLower(corpus);
    bool sawTerms = false;
    for (const std::vector<Term>& group : groups) {
        if (group.empty()) {
            continue;
        }
        sawTerms = true;
        const bool groupMatches = std::all_of(group.begin(), group.end(), [&](const Term& term) {
            const bool found = haystack.find(term.needle) != std::string::npos;
            return term.negated ? !found : found;
        });
        if (groupMatches) {
            return true;
        }
    }
    return !sawTerms;  // an empty (or null) expression matches everything
}

std::string collectSearchText(const codec::XmlNode& node) {
    std::string out;
    const auto append = [&out](const std::string& piece) {
        if (piece.empty()) {
            return;
        }
        if (!out.empty()) {
            out.push_back(' ');
        }
        out += piece;
    };
    if (node.kind == codec::XmlNodeKind::Text || node.kind == codec::XmlNodeKind::CData) {
        append(node.value);
        return out;
    }
    for (const codec::XmlNode& child : node.children) {
        append(collectSearchText(child));
    }
    return out;
}

// ---- public: ObjectQuery parse/build -----------------------------------

Result<ObjectQuery> parseObjectQuery(const codec::Message& message) {
    if (message.type() != codec::MessageType::MosReqObjList) {
        return makeError(ErrorCode::InvalidArgument, "not a mosReqObjList message");
    }
    const codec::XmlNode* body = message.body();
    if (body == nullptr) {
        return makeError(ErrorCode::ParseError, "mosReqObjList: empty message body");
    }

    ObjectQuery query;
    query.queryID = body->childText("queryID");
    query.username = std::string{body->attribute("username").value_or("")};
    query.generalSearch = body->childText("generalSearch");
    query.mosSchema = body->childText("mosSchema");

    const std::string startText = body->childText("listReturnStart");
    const std::optional<std::size_t> start = parseIndex(startText);
    if (!start.has_value() || *start < 1) {
        return makeError(ErrorCode::ParseError,
                         "mosReqObjList: listReturnStart must be a value of 1 or greater");
    }
    query.listReturnStart = *start;

    const std::string endText = body->childText("listReturnEnd");
    if (!endText.empty()) {
        const std::optional<std::size_t> end = parseIndex(endText);
        if (!end.has_value() || *end < *start) {
            return makeError(ErrorCode::ParseError,
                             "mosReqObjList: listReturnEnd must be >= listReturnStart");
        }
        query.listReturnEnd = *end;
    }

    // Sort-consistency bookkeeping (spec rules: a field name keeps the same
    // attributes everywhere; no two unlike fields share a sortByOrder).
    std::map<std::string, std::pair<int, bool>> sortByField;
    std::map<int, std::string> fieldByOrder;

    const codec::XmlNode& node = *body;
    for (const codec::XmlNode& groupNode : node.children) {
        if (!groupNode.isElement("searchGroup")) {
            continue;
        }
        ObjectQueryGroup group;
        for (const codec::XmlNode& fieldNode : groupNode.children) {
            if (!fieldNode.isElement("searchField")) {
                continue;
            }
            const std::optional<std::string_view> xpath = fieldNode.attribute("XPath");
            if (!xpath.has_value()) {
                return makeError(ErrorCode::ParseError,
                                 "mosReqObjList: searchField without an XPath attribute");
            }
            Result<FieldExpression> parsed = parseFieldXPath(*xpath);
            if (!parsed.ok()) {
                return std::move(parsed).error();
            }
            ObjectQueryField field;
            field.expression = std::move(parsed).value();
            if (const std::optional<std::string_view> order = fieldNode.attribute("sortByOrder")) {
                const std::optional<std::size_t> value = parseIndex(*order);
                if (!value.has_value()) {
                    return makeError(ErrorCode::ParseError,
                                     "mosReqObjList: sortByOrder is not a number");
                }
                field.sortByOrder = static_cast<int>(*value);
            }
            if (const std::optional<std::string_view> type = fieldNode.attribute("sortType")) {
                field.sortDescending = equalsIgnoreCase(*type, "DESCENDING");
            }

            const std::string& name = field.expression.sourceText;
            const auto [it, inserted] = sortByField.try_emplace(
                name, std::make_pair(field.sortByOrder, field.sortDescending));
            if (!inserted &&
                it->second != std::make_pair(field.sortByOrder, field.sortDescending)) {
                return makeError(ErrorCode::ParseError,
                                 "mosReqObjList: field '" + name +
                                     "' carries conflicting sort attributes across groups");
            }
            if (field.sortByOrder >= 0) {
                const auto [orderIt, orderInserted] =
                    fieldByOrder.try_emplace(field.sortByOrder, name);
                if (!orderInserted && orderIt->second != name) {
                    return makeError(ErrorCode::ParseError,
                                     "mosReqObjList: two unlike fields share sortByOrder " +
                                         std::to_string(field.sortByOrder));
                }
            }
            group.fields.push_back(std::move(field));
        }
        if (!group.fields.empty()) {
            query.groups.push_back(std::move(group));
        }
    }
    return query;
}

codec::Message buildMosReqObjList(const ObjectQuery& query) {
    codec::Message message = codec::makeMessage(codec::MessageType::MosReqObjList);
    if (codec::XmlNode* body = message.body()) {
        codec::view::MosReqObjList view{*body};
        if (!query.username.empty()) {
            view.setAttrUsername(query.username);
        }
        view.setQueryID(query.queryID);
        view.setListReturnStart(std::to_string(query.listReturnStart));
        view.setListReturnEnd(query.listReturnEnd.has_value() ? std::to_string(*query.listReturnEnd)
                                                              : "");
        view.setGeneralSearch(query.generalSearch);
        view.setMosSchema(query.mosSchema);
        for (const ObjectQueryGroup& group : query.groups) {
            codec::view::SearchGroup groupView{view.addSearchGroup()};
            for (const ObjectQueryField& field : group.fields) {
                codec::view::SearchField fieldView{groupView.addSearchField()};
                fieldView.setAttrXPath(field.expression.sourceText);
                if (field.sortByOrder >= 0) {
                    fieldView.setAttrSortByOrder(std::to_string(field.sortByOrder));
                }
                if (field.sortDescending) {
                    fieldView.setAttrSortType("DESCENDING");
                }
            }
        }
    }
    return message;
}

codec::Message buildMosObjList(const ObjectQuery& query, const ObjectQueryPage& page) {
    codec::Message message = codec::makeMessage(codec::MessageType::MosObjList);
    if (codec::XmlNode* body = message.body()) {
        codec::view::MosObjList view{*body};
        if (!query.username.empty()) {
            view.setAttrUsername(query.username);
        }
        view.setQueryID(query.queryID);
        view.setListReturnStart(std::to_string(page.listReturnStart));
        view.setListReturnEnd(std::to_string(page.listReturnEnd));
        view.setListReturnTotal(std::to_string(page.listReturnTotal));
        if (!page.listReturnStatus.empty()) {
            view.setListReturnStatus(page.listReturnStatus);
        }
        if (!page.objects.empty()) {
            codec::view::List list{view.ensureList()};
            for (const codec::Message& object : page.objects) {
                if (const codec::XmlNode* objBody = object.body()) {
                    list.addMosObj() = *objBody;  // verbatim <mosObj> subtree
                }
            }
        }
    }
    return message;
}

// ---- public: evaluateQuery ---------------------------------------------

Result<ObjectQueryPage> evaluateQuery(const ObjectQuery& query,
                                      const std::vector<const codec::Message*>& corpus) {
    if (query.listReturnStart < 1) {
        return makeError(ErrorCode::InvalidArgument, "listReturnStart must be 1 or greater");
    }
    if (query.listReturnEnd.has_value() && *query.listReturnEnd < query.listReturnStart) {
        return makeError(ErrorCode::InvalidArgument, "listReturnEnd must be >= listReturnStart");
    }

    struct Match {
        const codec::Message* message = nullptr;
        std::vector<const codec::XmlNode*> roots;
    };
    std::vector<Match> matched;
    for (const codec::Message* candidate : corpus) {
        if (candidate == nullptr || candidate->type() != codec::MessageType::MosObj) {
            continue;
        }
        const codec::XmlNode* mosObj = candidate->body();
        if (mosObj == nullptr) {
            continue;
        }
        if (!query.generalSearch.empty() &&
            !evaluateGeneralSearch(query.generalSearch, collectSearchText(*mosObj))) {
            continue;
        }
        std::vector<const codec::XmlNode*> roots = payloadRoots(*mosObj, query.mosSchema);
        if (!query.groups.empty()) {
            const bool anyGroup = std::any_of(
                query.groups.begin(), query.groups.end(), [&](const ObjectQueryGroup& group) {
                    return std::all_of(
                        group.fields.begin(), group.fields.end(),
                        [&](const ObjectQueryField& field) { return fieldMatches(field, roots); });
                });
            if (!anyGroup) {
                continue;
            }
        }
        matched.push_back(Match{candidate, std::move(roots)});
    }

    const std::vector<SortSpec> plan = sortPlan(query);
    if (!plan.empty()) {
        std::stable_sort(matched.begin(), matched.end(), [&](const Match& a, const Match& b) {
            for (const SortSpec& spec : plan) {
                const std::string va = sortValue(spec.field->expression, a.roots);
                const std::string vb = sortValue(spec.field->expression, b.roots);
                if (va == vb) {
                    continue;
                }
                const double na = toNumber(va);
                const double nb = toNumber(vb);
                bool less = false;
                if (!std::isnan(na) && !std::isnan(nb)) {
                    less = na < nb;
                } else {
                    less = va < vb;
                }
                return spec.field->sortDescending ? !less : less;
            }
            return false;  // full tie: keep corpus order (stable sort)
        });
    }

    ObjectQueryPage page;
    page.listReturnTotal = matched.size();
    const std::size_t start = query.listReturnStart;
    const std::size_t end = std::min(query.listReturnEnd.value_or(matched.size()), matched.size());
    if (page.listReturnTotal == 0 || start > end) {
        // Zero total (or a page past the end): start/end report zero and the
        // status says why (spec: listReturnStatus explains a zero total).
        if (page.listReturnTotal == 0) {
            page.listReturnStatus = "no objects matched the query";
        }
        return page;
    }
    page.listReturnStart = start;
    page.listReturnEnd = end;
    page.objects.reserve(end - start + 1);
    for (std::size_t i = start; i <= end; ++i) {
        page.objects.push_back(*matched[i - 1].message);
    }
    return page;
}

}  // namespace imos::profiles
