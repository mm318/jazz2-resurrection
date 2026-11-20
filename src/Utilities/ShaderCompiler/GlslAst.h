#pragma once

/**
	@file GlslAst.h

	Shared GLSL expression AST and stage tokenizer for the AST-based ShaderCompiler lowerings.

	The desktop shader emitters share expression nodes, the stage lexer and the preprocessor.
	Typed statements and declarations are defined in GlslTypedAst.h.
*/

#include <cstddef>
#include <cstdint>
#include <memory>

#include <Containers/SmallVector.h>
#include <Containers/StringConcatenable.h>

#include "ConstFold.h" // GlslToken, GlslTokenType, GlslExprTokenizer (+ String/StringView via Death::Containers)
#include "ShaderParser.h" // SourceLine, Diagnostic, Preprocessor, ShaderParser

namespace ShaderCompiler
{
	// ── Shared Lexical / Precedence Helpers ──────────────────────────────────────────────────────

	/** @brief True for a type-qualifier keyword the AST parsers skip (precision / interpolation / const) */
	inline bool IsQualifier(StringView k)
	{
		return (k == "const" || k == "highp" || k == "mediump" || k == "lowp" ||
			k == "flat" || k == "smooth" || k == "invariant" || k == "centroid" || k == "noperspective");
	}

	/**
		Precedence of a binary/assignment operator, mirroring ConstFold's model (assignment = 1,
		ternary handled separately at 2). Returns -1 for anything that is not a binary operator.
	*/
	inline std::int32_t BinPrec(StringView op)
	{
		if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" ||
			op == "%=" || op == "&=" || op == "|=" || op == "^=" || op == "<<=" || op == ">>=")
			return 1;
		if (op == "||")
			return 3;
		if (op == "^^")
			return 4;
		if (op == "&&")
			return 5;
		if (op == "|")
			return 6;
		if (op == "^")
			return 7;
		if (op == "&")
			return 8;
		if (op == "==" || op == "!=")
			return 9;
		if (op == "<" || op == ">" || op == "<=" || op == ">=")
			return 10;
		if (op == "<<" || op == ">>")
			return 11;
		if (op == "+" || op == "-")
			return 12;
		if (op == "*" || op == "/" || op == "%")
			return 13;
		return -1;
	}

	// ── Expression AST ───────────────────────────────────────────────────────────────────────────

	/** @brief Kind of an @ref Expr node (the union across the AST-based targets) */
	enum class ExprKind
	{
		IntLit,
		UIntLit,
		FloatLit,
		BoolLit,
		Ident,
		Member,
		Index,
		Call,
		Unary,
		Binary,
		Assign,
		Conditional
	};

	/** @brief One expression-tree node; the type-free shape shared by the AST-based lowerings */
	struct Expr
	{
		/** @brief Node kind */
		ExprKind Kind;
		/** @brief Literal text / identifier / member field / callee name / operator */
		String Text;
		/** @brief Unary only: `x++`/`x--` (operator follows the operand) vs. a prefix `++x` */
		bool Postfix = false;
		/** @brief Operands (Index: A = base, B = index; Conditional: A ? B : C) */
		std::unique_ptr<Expr> A, B, C;
		/** @brief Call/constructor arguments */
		SmallVector<std::unique_ptr<Expr>, 0> Args;
	};
	using ExprPtr = std::unique_ptr<Expr>;

	/** @brief Allocates an @ref Expr of the given kind, optionally carrying literal/identifier/operator text */
	inline ExprPtr MakeExpr(ExprKind kind, String text = {})
	{
		auto e = std::make_unique<Expr>();
		e->Kind = kind;
		e->Text = Death::move(text);
		return e;
	}

	/**
		Splits, strips comments from, preprocesses and tokenizes a GLSL stage source, appending a trailing
		@ref GlslTokenType::End token. Returns false (with a @p reason) only if preprocessing fails.
	*/
	inline bool TokenizeStage(StringView glsl, SmallVectorImpl<GlslToken>& tokens, String& reason)
	{
		using namespace Death::Containers::Literals;

		SmallVector<SourceLine, 0> lines;
		ShaderParser::SplitLines(glsl, lines);
		ShaderParser::StripComments(lines);

		Preprocessor preprocessor;
		SmallVector<SourceLine, 0> preprocessed;
		Diagnostic diag;
		if (!preprocessor.Run(lines, preprocessed, diag)) {
			reason = "preprocessing failed: "_s + diag.Message;
			return false;
		}
		for (std::size_t i = 0; i < preprocessed.size(); i++) {
			const String& text = preprocessed[i].Text;
			GlslExprTokenizer::Tokenize(text, 0, text.size(), i, tokens);
		}
		GlslToken end;
		end.Type = GlslTokenType::End;
		tokens.push_back(Death::move(end));
		return true;
	}
}
