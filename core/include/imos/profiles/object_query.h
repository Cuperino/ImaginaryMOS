// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 3 object search vocabulary (plan §3.2 row 3, milestone M5): the
// parsed form of <mosReqObjList> (ObjectQuery), its paged answer
// (ObjectQueryPage), and a library-provided evaluator so simple hosts get
// spec-correct search semantics over their in-memory catalog for free while
// database-backed hosts translate ObjectQuery themselves.
//
// Decision of record (profile3-plan §1.3): the spec's XPath subset is
// evaluated by a hand-rolled parser/evaluator over the retained XmlNode DOM.
// libxml2's XPath module (provisionally compiled in until this round) is not
// used — the codec's tree is our own XmlNode model, so libxml2 XPath would
// have required materializing a parallel libxml2 document per queried
// object; the subset below is small enough that the hand-rolled evaluator
// is both simpler and lighter (WASM footprint included).
//
// The subset, extracted from the spec's mosReqObjList section ("Basic XPath
// Expressions … Abbreviated Syntax for Location Paths with Location Steps
// that may contain Predicates"):
//
//   searchExpr   := locationPath [ '[' orExpr ']' ]
//   locationPath := ['/'] name ('/' name)*        (child steps only)
//   orExpr       := andExpr  (('or'|'OR')  andExpr)*
//   andExpr      := boolExpr (('and'|'AND') boolExpr)*
//   boolExpr     := 'not' '(' orExpr ')' | 'true' '()' | 'false' '()'
//                 | 'boolean' '(' orExpr ')' | comparison
//   comparison   := operand cmpOp operand
//                 | cmpOp operand                  (implicit '.' left side —
//                   the spec's own "[.>60 AND <120]" example)
//                 | operand                        (existence / truthiness)
//   cmpOp        := '=' | '!=' | '<' | '>' | '<=' | '>='
//   operand      := '.' | number | 'literal' | "literal"
//                 | 'string' '(' operand ')' | 'number' '(' operand ')'
//   name         := XML name characters (no axes, no wildcards, no '@')
//
// Comparison semantics follow XPath 1.0 within the subset: when either side
// is a number (literal or number()-converted), both sides convert to numbers
// (a non-numeric value yields NaN, which compares false); otherwise the
// comparison is an exact, case-sensitive string comparison ("XPath search
// requests are assumed to be case sensitive"). Keywords and function names
// are matched case-insensitively (the spec's examples spell AND in caps).
//
// Evaluation context (decision of record): a searchField's location path is
// anchored at the <mosPayload> element of a <mosExternalMetadata> block of
// the candidate object — the block whose <mosSchema> equals the query's
// <mosSchema> when one is given, any block otherwise. A field matches when
// any selected node satisfies the predicate in any eligible block. The
// leading '/' is optional and does not change the anchor (the spec's own
// example mixes "/Presenter […]" and "Producer […]" in one group).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imos/codec/message.h>
#include <imos/codec/xml.h>
#include <imos/core_export.h>
#include <imos/error.h>

namespace imos::profiles {

// ---- subset-XPath expression tree --------------------------------------

enum class XPathOp : std::uint8_t { Eq, Ne, Lt, Gt, Le, Ge };

/// One comparison operand: the context node's string-value ('.'), a quoted
/// string literal, or a numeric literal. string()/number() conversions
/// collapse into `forceString`/`forceNumber` on the wrapped operand.
struct XPathOperand {
    enum class Kind : std::uint8_t { Context, String, Number };
    Kind kind = Kind::Context;
    std::string text;     ///< String literal value
    double number = 0.0;  ///< Number literal value
    bool forceString = false;
    bool forceNumber = false;
};

/// A parsed predicate expression: OR/AND/NOT combinators over comparisons
/// (children) or one comparison/operand leaf.
struct XPathExpr {
    enum class Kind : std::uint8_t {
        Or,       ///< any child true
        And,      ///< every child true
        Not,      ///< single child negated
        Compare,  ///< lhs op rhs
        Truthy,   ///< bare operand: boolean() conversion
        True,     ///< true()
        False,    ///< false()
    };
    Kind kind = Kind::Compare;
    std::vector<XPathExpr> children;  ///< Or / And / Not / boolean()
    XPathOp op = XPathOp::Eq;         ///< Compare
    XPathOperand lhs;                 ///< Compare
    XPathOperand rhs;                 ///< Compare / Truthy
};

/// One parsed searchField XPath: the location path plus an optional
/// predicate. `sourceText` is the verbatim attribute value (what a builder
/// re-emits on the wire).
struct FieldExpression {
    std::vector<std::string> steps;
    std::optional<XPathExpr> predicate;
    std::string sourceText;
};

/// Parse a searchField XPath attribute against the subset grammar above.
IMOS_CORE_API Result<FieldExpression> parseFieldXPath(std::string_view xpath);

/// Evaluate one parsed field against a payload root: walk the steps from
/// `context` (each step selects matching child elements) and test the
/// predicate against each selected node's string-value; no predicate means
/// existence. True when any selected node matches.
[[nodiscard]] IMOS_CORE_API bool evaluateField(const FieldExpression& field,
                                               const codec::XmlNode& context);

// ---- generalSearch -----------------------------------------------------

/// Evaluate a <generalSearch> expression ("man", "man dog", "man and dog",
/// "man not dog", "man or dog", "man and dog not poodle") against a body of
/// text. Terms match by case-insensitive substring; adjacency and "and"
/// conjoin, "or" separates alternatives, "not" negates the following term.
/// An empty expression matches everything (the tag is mandatory even when
/// null). Operator words are recognized case-insensitively.
[[nodiscard]] IMOS_CORE_API bool evaluateGeneralSearch(std::string_view expression,
                                                       std::string_view corpus);

/// The concatenated text content of an element subtree (the default-field
/// corpus generalSearch runs over), whitespace-separated.
[[nodiscard]] IMOS_CORE_API std::string collectSearchText(const codec::XmlNode& node);

// ---- ObjectQuery / ObjectQueryPage -------------------------------------

/// One <searchField/>: the parsed XPath plus the spec's sort attributes
/// (sortByOrder is a priority rank; absent = the field does not sort).
struct ObjectQueryField {
    FieldExpression expression;
    int sortByOrder = -1;  ///< -1 = no sort role
    bool sortDescending = false;
};

/// One <searchGroup>: fields AND together; groups OR with each other.
struct ObjectQueryGroup {
    std::vector<ObjectQueryField> fields;
};

/// The parsed form of one <mosReqObjList> (spec §3.2.5). start/end are the
/// 1-based inclusive result window; a missing end means "all results".
struct ObjectQuery {
    std::string queryID;
    std::string username;
    std::size_t listReturnStart = 1;
    std::optional<std::size_t> listReturnEnd;
    std::string generalSearch;
    std::string mosSchema;
    std::vector<ObjectQueryGroup> groups;
};

/// One page of results, the content of a <mosObjList> (spec §3.2.6):
/// verbatim <mosObj> wrapper messages plus the paging metadata. start/end
/// are the actual 1-based indices returned (0/0 for an empty page).
struct ObjectQueryPage {
    std::vector<codec::Message> objects;
    std::size_t listReturnStart = 0;
    std::size_t listReturnEnd = 0;
    std::size_t listReturnTotal = 0;
    std::string listReturnStatus;
};

/// Parse a mosReqObjList message into an ObjectQuery (every searchField
/// XPath is parsed against the subset grammar; the first failure is the
/// error). Fails on a non-mosReqObjList message and on a listReturnStart
/// below 1 or an end below start (spec-mandated shape).
IMOS_CORE_API Result<ObjectQuery> parseObjectQuery(const codec::Message& message);

/// Build the wire mosReqObjList for `query`. Fields emit their verbatim
/// sourceText; the mandatory-even-when-null tags (queryID, listReturnStart,
/// listReturnEnd, generalSearch) are always present.
[[nodiscard]] IMOS_CORE_API codec::Message buildMosReqObjList(const ObjectQuery& query);

/// Build the wire mosObjList answering `query` from an evaluated page.
/// `page.objects` ride verbatim inside <list> (omitted when empty).
[[nodiscard]] IMOS_CORE_API codec::Message buildMosObjList(const ObjectQuery& query,
                                                           const ObjectQueryPage& page);

/// Evaluate `query` over an in-memory corpus of <mosObj> wrapper messages
/// with the spec semantics: generalSearch AND (groups ORed); deterministic
/// sort per the fields' sortByOrder/sortType (numeric-aware, ties keep
/// corpus order); then the start/end page slice. This is the free path
/// simple hosts get; database-backed hosts translate ObjectQuery instead.
IMOS_CORE_API Result<ObjectQueryPage> evaluateQuery(
    const ObjectQuery& query, const std::vector<const codec::Message*>& corpus);

}  // namespace imos::profiles
