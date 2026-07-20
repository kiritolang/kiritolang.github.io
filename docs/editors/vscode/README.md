# Kirito Language — VS Code extension

Language support for the [Kirito](https://github.com/kiritolang/kiritolang.github.io) scripting language (`.ki`).

## Features

- **Syntax highlighting** (TextMate grammar, `source.kirito`): keywords and logical operators,
  `True`/`False`/`None`/`self`, built-in types (incl. `Bytes`), builtin functions, dunder/special
  methods, standard-library module names, `#` comments, decimal/hex/octal/binary/float numbers, and
  every string flavour — single/double-quoted, triple-quoted, `f"…"`/`f'…'` f-strings with `{…}`
  interpolation and `:format-spec`, and `r"…"` raw strings (plus `rf`/`fr`), with escape sequences.
- **Language configuration**: `#` comment toggling, bracket and quote auto-closing/surrounding,
  off-side (indentation) folding, `#region`/`#endregion` markers, auto-indent after a `:` block
  header, and auto-dedent after `return`/`break`/`continue`/`pass`/`throw`.
- **Snippets**: `fn`, `fnt`, `lambda`, `class`, `classb`, `def`, `if`/`ife`/`ifee`, `for`, `while`,
  `try`/`tryf`, `switch`, `with`, `import`, `main`, `print`.

## Install

Local: copy this folder to your extensions dir and reload VS Code —

```sh
cp -r docs/editors/vscode ~/.vscode/extensions/kirito-language-0.2.1
# Windows: %USERPROFILE%\.vscode\extensions\kirito-language-0.2.1
```

Or package a `.vsix`:

```sh
npm install -g @vscode/vsce
vsce package
code --install-extension kirito-language-0.2.1.vsix
```

The grammar also works in any TextMate-grammar-aware tool (Sublime Text, the `bat` pager, …) via
`syntaxes/kirito.tmLanguage.json` (`scopeName: source.kirito`).
