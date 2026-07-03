" Vim syntax file for the Kirito language.
" Install:  copy to  ~/.vim/syntax/kirito.vim  and add to your vimrc:
"             autocmd BufRead,BufNewFile *.ki set filetype=kirito
"           (Neovim: ~/.config/nvim/syntax/kirito.vim)

if exists("b:current_syntax")
  finish
endif

syn keyword kiritoKeyword var Function class if elif else while for in break continue return
syn keyword kiritoKeyword try catch finally throw with as pass todo assert discard switch case default
syn keyword kiritoOperator and or not
syn keyword kiritoConstant True False None
syn keyword kiritoType Integer Float String Bool List Set Dict Array Any
syn keyword kiritoBuiltin abs all any bin bitand bitnot bitor bitxor chr divmod enumerate filter
syn keyword kiritoBuiltin format hex id import inspect isinstance len map max min oct ord pow range
syn keyword kiritoBuiltin reversed round shl shr sorted sum type zip

syn match   kiritoComment "#.*$" contains=@Spell
syn match   kiritoNumber  "\<0[xX]\x\+\>"
syn match   kiritoNumber  "\<0[oO]\o\+\>"
syn match   kiritoNumber  "\<0[bB][01]\+\>"
syn match   kiritoNumber  "\<\d\+\>"
syn match   kiritoNumber  "\<\d\+\.\d\+\([eE][-+]\?\d\+\)\?\>"
syn match   kiritoSpecial "\<_\(init\|str\|add\|sub\|mul\|div\|floordiv\|mod\|pow\|eq\|ne\|lt\|le\|gt\|ge\|neg\|not\|call\|getitem\|setitem\|len\|contains\|iter\|enter\|exit\|super\|getstate\|setstate\)_\>"

syn region  kiritoString  start=+"+ skip=+\\"+ end=+"+ contains=kiritoEscape
syn region  kiritoFString matchgroup=kiritoString start=+f"+ skip=+\\"+ end=+"+ contains=kiritoEscape,kiritoInterp
syn match   kiritoEscape  contained "\\\(x\x\{2}\|u\x\{4}\|U\x\{8}\|[nrtfvab0\\\"']\)"
syn region  kiritoInterp  contained matchgroup=kiritoDelim start=+{+ end=+}+ contains=kiritoNumber,kiritoBuiltin,kiritoConstant

hi def link kiritoKeyword  Statement
hi def link kiritoOperator Operator
hi def link kiritoConstant Constant
hi def link kiritoType     Type
hi def link kiritoBuiltin  Function
hi def link kiritoComment  Comment
hi def link kiritoNumber   Number
hi def link kiritoString   String
hi def link kiritoFString  String
hi def link kiritoEscape   SpecialChar
hi def link kiritoSpecial  Identifier
hi def link kiritoDelim    Delimiter

let b:current_syntax = "kirito"
