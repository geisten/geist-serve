'use strict';
// Marked supplies syntax tokens only; model HTML is always literal text.
// Math is rendered by pinned, untrusted-mode KaTeX into native MathML DOM.
const chatMarkdown = (() => {
  const mathCaches = new WeakMap();
  // Limits apply per expression and per message render. Cache only the current
  // message's formulas, so streaming does not reparse already complete math.
  const MAX_MATH_LENGTH = 8192, MAX_MATH_COUNT = 64;
  function mathToken(source, type) {
    const open = ['$$', '\\[', '\\(', '$'].find(value => source.startsWith(value));
    if (!open) return;
    const display = open === '$$' || open === '\\[';
    if (type === 'math_block' && !display) return;
    const close = open === '\\[' ? '\\]' : open === '\\(' ? '\\)' : open;
    if (open === '$' && (!source[1] || /\s|\$/.test(source[1]))) return;
    let end = -1, groupDepth = 0;
    for (let i = open.length; i < source.length; i++) {
      if (open === '$' && source[i] === '\n') break;
      // An unmatched currency symbol must not consume a later code span.
      if (open === '$' && source[i] === '`' && groupDepth === 0) break;
      if (source.startsWith(close, i)) { end = i; break; }
      if (source[i] === '\\') { i++; continue; }
      if (source[i] === '{') groupDepth++;
      if (source[i] === '}') groupDepth--;
    }
    if (end < 0) {
      // Keep unclosed delimiters literal while tokens arrive. A lone dollar
      // can be currency; let the ordinary Markdown lexer handle it.
      if (open !== '$') return {type, raw: source, text: source, pending: true, display};
      return;
    }
    // Common currency pairs such as "$5 and $10" are not inline math.
    if (open === '$' && (/\s/.test(source[end - 1]) || /[\d$]/.test(source[end + 1] || ''))) return;
    const raw = source.slice(0, end + close.length);
    return {type, raw, text: source.slice(open.length, end), display};
  }
  const lexer = new marked.Marked({gfm: true, breaks: true, extensions: [
    {name: 'math_block', level: 'block',
      start: source => source.search(/(?:^|\n)(?:\$\$|\\\[)/),
      tokenizer: source => mathToken(source, 'math_block')},
    {name: 'math_inline', level: 'inline',
      start: source => source.search(/\$|\\[([]/),
      tokenizer: source => mathToken(source, 'math_inline')}
  ]});
  const decoder = document.createElement('textarea');
  const entities = text => text.replace(/&(?:#\d{1,7}|#x[\da-f]{1,6}|[a-z][a-z\d]{1,31});/gi, entity => {
    // The match contains no tags, quotes or delimiters outside one entity.
    decoder.innerHTML = entity;
    return decoder.value;
  });
  const element = (tag, className) => {
    const node = document.createElement(tag);
    if (className) node.className = className;
    return node;
  };
  function mathInto(parent, token, context) {
    const node = element(token.type === 'math_block' ? 'div' : 'span', token.display ? 'math-display' : 'math-inline');
    if (token.pending) {
      node.classList.add('math-source'); node.textContent = token.raw;
      parent.append(node); return;
    }
    try {
      if (++context.count > MAX_MATH_COUNT || token.text.length > MAX_MATH_LENGTH) throw new Error('Math limit');
      let depth = 0;
      for (let i = 0; i < token.text.length; i++) {
        if (token.text[i] === '\\') { i++; continue; }
        if (token.text[i] === '{' && ++depth > 64) throw new Error('Math nesting limit');
        if (token.text[i] === '}') depth--;
      }
      const key = `${token.display ? 'display' : 'inline'}:${token.text}`;
      let rendered = context.previous.get(key);
      if (!rendered) {
        rendered = element('span');
        katex.render(token.text, rendered, {
          displayMode: token.display, output: 'mathml', throwOnError: true,
          trust: false, strict: 'error', maxExpand: 1000, maxSize: 10,
          macros: {} // Definitions must never leak to a later expression/message.
        });
      }
      context.next.set(key, rendered);
      node.append(...[...rendered.childNodes].map(child => child.cloneNode(true)));
      if (token.display) {
        node.tabIndex = 0; node.setAttribute('role', 'region');
        node.dataset.uiLabel = 'Formula'; node.ariaLabel = t('Formula');
      }
    } catch {
      // Never insert a parser error as HTML. Preserve unknown/invalid LaTeX
      // exactly, including its delimiters, instead of dropping the answer.
      node.classList.add('math-source'); node.textContent = token.raw;
      node.dataset.uiTitle = 'Formula shown as source'; node.title = t('Formula shown as source');
    }
    parent.append(node);
  }
  function copyButton(value, label) {
    const button = element('button', 'markdown-copy');
    button.type = 'button'; button._markdownCopyText = value;
    button.dataset.uiTitle = button.dataset.uiLabel = label;
    button.title = button.ariaLabel = t(label);
    const icon = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    icon.setAttribute('viewBox', '0 0 24 24'); icon.setAttribute('aria-hidden', 'true');
    const path = document.createElementNS(icon.namespaceURI, 'path');
    path.setAttribute('d', 'M9 9h11v11H9zM15 5V3H3v12h2');
    icon.append(path); button.append(icon);
    return button;
  }
  function tokensInto(parent, tokens, depth = 0, context) {
    if (depth > 64) throw new Error('Markdown nesting limit');
    for (const token of tokens) {
      let node;
      switch (token.type) {
        case 'math_inline': case 'math_block': mathInto(parent, token, context); continue;
        case 'space': case 'def': continue;
        case 'heading': node = element(`h${Math.min(6, token.depth + 2)}`); break;
        case 'paragraph': node = element('p'); break;
        case 'strong': case 'em': case 'del': node = element(token.type); break;
        case 'blockquote': node = element('blockquote'); break;
        case 'br': parent.append(element('br')); continue;
        case 'hr': parent.append(element('hr')); continue;
        case 'codespan':
          node = element('code'); node.textContent = token.text; parent.append(node); continue;
        case 'code': {
          node = element('div', 'code-block');
          const toolbar = element('div', 'code-toolbar');
          const label = element('span'); label.textContent = token.lang?.split(/\s/)[0] || t('Code');
          toolbar.append(label, copyButton(token.text, 'Copy code'));
          const pre = element('pre'); pre.tabIndex = 0; pre.dataset.uiLabel = 'Code'; pre.ariaLabel = t('Code');
          const code = element('code'); code.textContent = token.text; pre.append(code);
          node.append(toolbar, pre); parent.append(node); continue;
        }
        case 'list':
          node = element(token.ordered ? 'ol' : 'ul');
          if (token.ordered) node.start = token.start;
          for (const item of token.items) {
            const li = element('li'); tokensInto(li, item.tokens, depth + 1, context); node.append(li);
          }
          parent.append(node); continue;
        case 'checkbox':
          node = element('span', 'task-check'); node.textContent = token.checked ? '☑ ' : '☐ ';
          node.setAttribute('role', 'img'); node.dataset.uiLabel = token.checked ? 'Checked' : 'Unchecked'; node.ariaLabel = t(node.dataset.uiLabel);
          parent.append(node); continue;
        case 'table': {
          node = element('div', 'table-scroll'); node.tabIndex = 0;
          node.setAttribute('role', 'region'); node.dataset.uiLabel = 'Table'; node.ariaLabel = t('Table');
          const table = element('table'), head = element('thead'), body = element('tbody');
          const row = (cells, header) => {
            const tr = element('tr');
            for (const cell of cells) {
              const td = element(header ? 'th' : 'td');
              if (header) td.scope = 'col';
              if (['left', 'right', 'center'].includes(cell.align)) td.style.textAlign = cell.align;
              tokensInto(td, cell.tokens, depth + 1, context); tr.append(td);
            }
            return tr;
          };
          head.append(row(token.header, true));
          for (const cells of token.rows) body.append(row(cells, false));
          table.append(head, body); node.append(table); parent.append(node); continue;
        }
        case 'link': case 'image': {
          node = element('span', 'markdown-reference');
          if (token.type === 'image') {
            const label = element('span'); label.dataset.uiText = 'Image'; label.textContent = t('Image');
            node.append(label, `: ${entities(token.text || '')}`);
          }
          else tokensInto(node, token.tokens, depth + 1, context);
          // Display/copy addresses, never navigate or fetch model-provided URLs.
          // The native shell's external-navigation allowlist stays unchanged.
          let url;
          try {
            url = new URL(entities(token.href));
            if (!['https:', 'http:'].includes(url.protocol) || url.username || url.password) url = null;
          } catch { url = null; }
          if (url) {
            const address = element('span', 'reference-address'); address.textContent = ` (${url.href})`;
            node.append(address, copyButton(url.href, 'Copy link'));
          }
          parent.append(node); continue;
        }
        case 'html':
          node = element(token.block ? 'p' : 'span', 'literal-html');
          node.textContent = token.text; parent.append(node); continue;
        default:
          if (token.tokens) tokensInto(parent, token.tokens, depth + 1, context);
          else parent.append(document.createTextNode(entities(token.text ?? token.raw ?? '')));
          continue;
      }
      tokensInto(node, token.tokens || [], depth + 1, context); parent.append(node);
    }
  }
  // Reuse unchanged nodes during streaming: selections, code/table scroll
  // positions and keyboard focus must survive the next token.
  function reconcile(parent, source) {
    for (let i = 0; i < source.childNodes.length; i++) {
      const next = source.childNodes[i], old = parent.childNodes[i];
      if (!old) { parent.append(next.cloneNode(true)); copyProperties(parent.lastChild, next); continue; }
      if (old.nodeType !== next.nodeType || old.nodeName !== next.nodeName) {
        const replacement = next.cloneNode(true); copyProperties(replacement, next); old.replaceWith(replacement); continue;
      }
      if (old.nodeType === Node.TEXT_NODE) {
        if (old.data !== next.data) {
          let prefix = 0;
          while (prefix < old.length && prefix < next.length && old.data[prefix] === next.data[prefix]) prefix++;
          old.replaceData(prefix, old.length - prefix, next.data.slice(prefix));
        }
      } else {
        old._markdownCopyText = next._markdownCopyText;
        for (const attr of [...old.attributes]) if (!next.hasAttribute(attr.name)) old.removeAttribute(attr.name);
        for (const attr of next.attributes) if (old.getAttribute(attr.name) !== attr.value) old.setAttribute(attr.name, attr.value);
        reconcile(old, next);
      }
    }
    while (parent.childNodes.length > source.childNodes.length) parent.lastChild.remove();
  }
  function copyProperties(target, source) {
    target._markdownCopyText = source._markdownCopyText;
    for (let i = 0; i < target.childNodes.length; i++) copyProperties(target.childNodes[i], source.childNodes[i]);
  }
  function updateMathOverflow(target) {
    for (const node of target.querySelectorAll('.math-inline')) {
      // Only overflowing inline formulas add a tab stop. A short arrow should
      // not make ordinary prose tedious to navigate by keyboard.
      if (node.scrollWidth > node.clientWidth + 1) {
        node.tabIndex = 0; node.setAttribute('role', 'region');
        node.dataset.uiLabel = 'Formula'; node.ariaLabel = t('Formula');
      } else {
        for (const attr of ['tabindex', 'role', 'data-ui-label', 'aria-label']) node.removeAttribute(attr);
      }
    }
  }
  function render(target, source) {
    const fragment = document.createDocumentFragment();
    const context = {previous: mathCaches.get(target) || new Map(), next: new Map(), count: 0};
    try { tokensInto(fragment, lexer.lexer(source), 0, context); }
    catch {
      // Preserve all text if unusually nested/malformed input cannot be parsed.
      const fallback = element('div', 'markdown-fallback'); fallback.textContent = source; fragment.replaceChildren(fallback);
    }
    mathCaches.set(target, context.next);
    reconcile(target, fragment);
    updateMathOverflow(target);
  }
  window.addEventListener('resize', () => updateMathOverflow(document));
  return {render};
})();
document.addEventListener('click', async event => {
  const button = event.target.closest('button.markdown-copy');
  if (!button) return;
  try {
    await copyText(button._markdownCopyText);
    uiText(document.getElementById('chat-announcement'), 'Copied');
    button.dataset.uiTitle = 'Copied'; button.title = t('Copied');
  } catch { message('Copy is unavailable here. Select the result and copy it manually.'); }
});
