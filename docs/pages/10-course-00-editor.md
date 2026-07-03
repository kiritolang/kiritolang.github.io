# Lesson 0 — Setting Up Your Editor

Before the first line of code, spend two minutes giving your editor **Kirito syntax highlighting**.
It's optional — Kirito runs fine from a plain text file — but coloured keywords, strings, and
comments make the rest of this course much easier to read. Kirito is young, so no editor knows it out
of the box; the repository ships ready-to-install definitions under [`docs/editors/`](https://github.com/AzethMeron/KiritoLang/tree/main/docs/editors).

Pick whichever editor you use.

## VS Code (recommended)

The `docs/editors/vscode/` folder is a complete extension — a grammar (keywords, types, builtins,
dunder methods, stdlib modules, and every string flavour: single/double/triple, `f"…"` with `{…}`
interpolation, and `r"…"` raw), plus comment-toggling, bracket/quote auto-closing, off-side folding,
auto-indent after `:`, and **snippets** (type `fn`, `class`, `for`, `try`, `switch`, `main`, … + Tab).

```text
cp -r docs/editors/vscode ~/.vscode/extensions/kirito-language-0.2.0
```

(On Windows that's `%USERPROFILE%\.vscode\extensions\kirito-language-0.2.0`.) Reload VS Code and any
`.ki` file lights up. To share it, package a `.vsix` with `vsce package` and
`code --install-extension`.

## Notepad++

Notepad++ has a built-in **User Defined Language** system, so no plugin is needed:

Pick the colour file matching your Notepad++ theme — `docs/editors/notepad++/kirito-dark.xml` (dark) or
`kirito-light.xml` (light):

1. `Language` menu → `User Defined Language` → `Define your language…`
2. Click `Import…` and choose the file for your theme.
3. Restart Notepad++.

`.ki` files are then highlighted automatically (or pick the matching **Kirito (Dark/Light)** entry
from the `Language` menu).

> Switching themes? Re-import the other file and restart Notepad++.

## Vim / Neovim

Copy `docs/editors/vim/kirito.vim` to `~/.vim/syntax/kirito.vim` (Neovim:
`~/.config/nvim/syntax/kirito.vim`) and tell Vim to use it for `.ki`:

```vim
autocmd BufRead,BufNewFile *.ki set filetype=kirito
```

## Anything else (the 30-second fallback)

If your editor has no Kirito mode, point it at any highlighter built for an **indentation-based
language with `#` line comments** — most editors let you map an extension to an existing mode — and
you get reasonable colouring instantly. The dedicated definitions above go further: they add the
Kirito keywords (`Function`, `var`, `switch`/`case`, `catch`/`throw`, `todo`, `discard`) and reflect
that `print`/`input` aren't builtins — they live in the `io` module.

## What you learned

- Where to find Kirito editor support (`docs/editors/`), and how to install it for VS Code, Notepad++, and
  Vim.
- The quick fallback for any other editor: map `.ki` to any indentation-based, `#`-comment mode.

With colours in place, let's write some code.
