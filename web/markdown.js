'use strict';
// Marked supplies syntax tokens only. Model output is never passed to an HTML
// renderer. Every element/attribute below is owned by the application.
const chatMarkdown = (() => {
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
  function copyButton(value, label) {
    const button = element('button', 'markdown-copy');
    button.type = 'button'; button._markdownCopyText = value;
    button.title = button.ariaLabel = t(label);
    const icon = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    icon.setAttribute('viewBox', '0 0 24 24'); icon.setAttribute('aria-hidden', 'true');
    const path = document.createElementNS(icon.namespaceURI, 'path');
    path.setAttribute('d', 'M9 9h11v11H9zM15 5V3H3v12h2');
    icon.append(path); button.append(icon);
    return button;
  }
  function tokensInto(parent, tokens, depth = 0) {
    if (depth > 64) throw new Error('Markdown nesting limit');
    for (const token of tokens) {
      let node;
      switch (token.type) {
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
          const pre = element('pre'); pre.tabIndex = 0; pre.ariaLabel = t('Code');
          const code = element('code'); code.textContent = token.text; pre.append(code);
          node.append(toolbar, pre); parent.append(node); continue;
        }
        case 'list':
          node = element(token.ordered ? 'ol' : 'ul');
          if (token.ordered) node.start = token.start;
          for (const item of token.items) {
            const li = element('li'); tokensInto(li, item.tokens, depth + 1); node.append(li);
          }
          parent.append(node); continue;
        case 'checkbox':
          node = element('span', 'task-check'); node.textContent = token.checked ? '☑ ' : '☐ ';
          node.setAttribute('role', 'img'); node.ariaLabel = t(token.checked ? 'Checked' : 'Unchecked');
          parent.append(node); continue;
        case 'table': {
          node = element('div', 'table-scroll'); node.tabIndex = 0;
          node.setAttribute('role', 'region'); node.ariaLabel = t('Table');
          const table = element('table'), head = element('thead'), body = element('tbody');
          const row = (cells, header) => {
            const tr = element('tr');
            for (const cell of cells) {
              const td = element(header ? 'th' : 'td');
              if (header) td.scope = 'col';
              if (['left', 'right', 'center'].includes(cell.align)) td.style.textAlign = cell.align;
              tokensInto(td, cell.tokens, depth + 1); tr.append(td);
            }
            return tr;
          };
          head.append(row(token.header, true));
          for (const cells of token.rows) body.append(row(cells, false));
          table.append(head, body); node.append(table); parent.append(node); continue;
        }
        case 'link': case 'image': {
          node = element('span', 'markdown-reference');
          if (token.type === 'image') node.append(`${t('Image')}: ${entities(token.text || '')}`);
          else tokensInto(node, token.tokens, depth + 1);
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
          if (token.tokens) tokensInto(parent, token.tokens, depth + 1);
          else parent.append(document.createTextNode(entities(token.text ?? token.raw ?? '')));
          continue;
      }
      tokensInto(node, token.tokens || [], depth + 1); parent.append(node);
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
  function render(target, source) {
    const fragment = document.createDocumentFragment();
    try { tokensInto(fragment, marked.lexer(source, {gfm: true, breaks: true})); }
    catch {
      // Preserve all text if unusually nested/malformed input cannot be parsed.
      const fallback = element('div', 'markdown-fallback'); fallback.textContent = source; fragment.replaceChildren(fallback);
    }
    reconcile(target, fragment);
  }
  return {render};
})();
document.addEventListener('click', async event => {
  const button = event.target.closest('button.markdown-copy');
  if (!button) return;
  try {
    await copyText(button._markdownCopyText);
    document.getElementById('chat-announcement').textContent = t('Copied');
    button.title = t('Copied');
  } catch { message('Copy is unavailable here. Select the result and copy it manually.'); }
});
