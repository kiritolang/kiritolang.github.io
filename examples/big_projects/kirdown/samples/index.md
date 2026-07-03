# Welcome to kirdown

**kirdown** is a small Markdown → HTML converter written in pure Kirito.

It handles the essentials:

- headings (`#` to `######`)
- bold **strong** and italic *emphatic*
- inline `code` with backticks
- fenced code blocks (with a language class)
- unordered and ordered lists
- [links](guide.html) and images
- horizontal rules
- blockquotes

## A quick example

```kirito
var kirdown = import("kirdown")
var body = kirdown.toBody("# hi\nworld")
```

> Everything HTML-special is escaped, so `<script>` in a paragraph is safe.

For usage, see the [guide](guide.html).

---

Built with Kirito.
