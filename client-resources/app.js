// ============================================================
// LAN Chat P2P — Client Application JS
// ============================================================

const native = {
  send(action, data = {}) {
    try {
      window.webkit.messageHandlers.nativeAPI.postMessage(
        JSON.stringify({ action, ...data })
      );
    } catch (e) { console.error(e); }
  }
};

// ---- App State ----
let state = {
  username: '',
  activeContact: null,
  contacts: {},  // { name: { online, unread, lastMsg } }
  messages: {},  // { contact: [ { id, from, to, content, timestamp, status } ] }
  loggedIn: false,
};

// ============================================================
// Called by C++ (from io_context thread via g_idle_add)
// ============================================================
window.__onLoginResult = function(success, errorMsg) {
  if (success) {
    state.loggedIn = true;
    document.getElementById('login-screen').classList.remove('active');
    document.getElementById('chat-screen').classList.add('active');
    document.getElementById('my-username').textContent = '👤 ' + state.username;
    document.getElementById('message-input').focus();
  } else {
    document.getElementById('login-error').textContent = errorMsg || '登录失败';
  }
  document.getElementById('login-btn').disabled = false;
  document.getElementById('login-btn').textContent = '进入聊天';
};

window.__onPeerOnline = function(name) {
  if (name === state.username) return;
  if (!state.contacts[name]) {
    state.contacts[name] = { online: true, unread: 0, lastMsg: '' };
  } else {
    state.contacts[name].online = true;
  }
  renderContactList();
  if (state.activeContact === name) updateChatHeader();
};

window.__onPeerOffline = function(name) {
  if (state.contacts[name]) {
    state.contacts[name].online = false;
    renderContactList();
    if (state.activeContact === name) updateChatHeader();
  }
};

window.__onMessage = function(jsonStr) {
  try {
    const msg = JSON.parse(jsonStr);
    handleMessage(msg);
  } catch (e) { console.error(e); }
};

// ============================================================
// Message handling
// ============================================================
function handleMessage(msg) {
  if (msg.type === 'chat_message') {
    addMessage(msg);
    const contact = msg.from === state.username ? msg.to : msg.from;
    if (!contact) return;

    if (!state.contacts[contact]) {
      state.contacts[contact] = { online: true, unread: 0, lastMsg: '' };
    }
    state.contacts[contact].lastMsg = msg.content;

    if (contact !== state.activeContact && msg.from !== state.username) {
      state.contacts[contact].unread = (state.contacts[contact].unread || 0) + 1;
      native.send('notify', { title: msg.from, body: msg.content });
    }

    renderContactList();
    if (state.activeContact === contact || (msg.from === state.username && state.activeContact === msg.to)) {
      renderMessages(state.activeContact);
    }
  } else if (msg.type === 'message_ack') {
    setMessageStatus(msg.id, 'delivered');
    if (state.activeContact) renderMessages(state.activeContact);
  } else if (msg.type === 'read_receipt') {
    setMessageStatus(msg.id, 'read');
    if (state.activeContact) renderMessages(state.activeContact);
  }
}

function addMessage(msg) {
  const contact = msg.from === state.username ? msg.to : msg.from;
  const key = contact || '__broadcast__';
  if (!state.messages[key]) state.messages[key] = [];
  if (state.messages[key].some(m => m.id === msg.id)) return;
  state.messages[key].push({
    id: msg.id, from: msg.from, to: msg.to,
    content: msg.content, timestamp: msg.timestamp,
    status: 'delivered'
  });
  state.messages[key].sort((a, b) => a.timestamp - b.timestamp);
}

function setMessageStatus(msgId, status) {
  for (const key in state.messages) {
    for (const m of state.messages[key]) {
      if (m.id === msgId) { m.status = status; return; }
    }
  }
}

// ============================================================
// Login
// ============================================================
function doLogin() {
  const username = document.getElementById('username-input').value.trim();
  if (!username) { document.getElementById('login-error').textContent = '请输入用户名'; return; }
  if (!/^[a-zA-Z0-9_一-鿿]{1,32}$/.test(username)) {
    document.getElementById('login-error').textContent = '用户名只能包含字母、数字、下划线和中文';
    return;
  }

  state.username = username;
  document.getElementById('login-error').textContent = '';
  document.getElementById('login-btn').disabled = true;
  document.getElementById('login-btn').textContent = '启动中...';

  native.send('login', { username: username });
}

document.getElementById('username-input').addEventListener('keydown', function(e) {
  if (e.key === 'Enter') doLogin();
});

// ============================================================
// Send message
// ============================================================
function sendCurrentMessage() {
  if (!state.activeContact) return;
  const input = document.getElementById('message-input');
  const content = input.value.trim();
  if (!content) return;

  const msgId = 'local-' + Date.now() + '-' + Math.random().toString(36).substr(2, 9);
  const contact = state.activeContact;

  if (!state.messages[contact]) state.messages[contact] = [];
  state.messages[contact].push({
    id: msgId, from: state.username, to: contact,
    content: content, timestamp: Date.now(), status: 'sent'
  });

  if (state.contacts[contact]) state.contacts[contact].lastMsg = content;

  input.value = '';
  renderMessages(contact);
  renderContactList();
  scrollToBottom();

  native.send('send_message', { to: contact, content: content });
}

// ============================================================
// Contact selection
// ============================================================
function selectContact(name) {
  state.activeContact = name;
  if (state.contacts[name]) {
    state.contacts[name].unread = 0;
    native.send('mark_read', { sender: name });
  }
  renderContactList();
  renderMessages(name);
  updateChatHeader();
  document.getElementById('message-input').focus();
}

// ============================================================
// Rendering
// ============================================================
function renderContactList() {
  const container = document.getElementById('contact-list');
  const names = Object.keys(state.contacts).sort((a, b) => {
    if (state.contacts[a].online !== state.contacts[b].online)
      return state.contacts[b].online ? 1 : -1;
    return a.localeCompare(b);
  });

  if (names.length === 0) {
    container.innerHTML = '<div class="system-msg" style="padding:24px;">等待其他用户上线...</div>';
    return;
  }

  container.innerHTML = names.map(name => {
    const c = state.contacts[name];
    const active = state.activeContact === name ? ' active' : '';
    const status = c.online ? 'online' : 'offline';
    const badge = c.unread > 0 ? `<span class="contact-badge">${c.unread > 99 ? '99+' : c.unread}</span>` : '';
    return `
      <div class="contact-item${active}" onclick="selectContact('${escAttr(name)}')">
        <div class="contact-avatar">
          ${name.charAt(0).toUpperCase()}
          <span class="contact-status ${status}"></span>
        </div>
        <div class="contact-info">
          <div class="contact-name">${escHtml(name)}</div>
          <div class="contact-preview">${escHtml(c.lastMsg || '').substring(0, 30)}</div>
        </div>
        ${badge}
      </div>
    `;
  }).join('');
}

function renderMessages(contact) {
  const container = document.getElementById('messages-list');
  const msgs = state.messages[contact] || [];

  if (msgs.length === 0) {
    document.getElementById('chat-title').textContent = contact;
    let ph = document.querySelector('.welcome-placeholder');
    if (!ph) {
      ph = document.createElement('div');
      ph.className = 'welcome-placeholder';
      ph.innerHTML = '<div class="icon">💬</div><div class="text">开始和 ' + escHtml(contact) + ' 聊天吧</div>';
      document.querySelector('.chat-area').insertBefore(ph, document.getElementById('input-area'));
    }
    return;
  }

  let ph = document.querySelector('.welcome-placeholder');
  if (ph) ph.remove();

  let lastDate = '';
  container.innerHTML = msgs.map(m => {
    const date = formatDate(m.timestamp);
    let sep = date !== lastDate ? `<div class="date-separator">${date}</div>` : '';
    lastDate = date;

    const mine = m.from === state.username;
    const time = formatTime(m.timestamp);
    const statusIcon = mine ? getStatusIcon(m.status) : '';

    return sep + `
      <div class="message-row${mine ? ' mine' : ''}">
        <div class="message-bubble">
          ${!mine ? `<div class="message-sender">${escHtml(m.from)}</div>` : ''}
          <div class="message-text">${formatContent(m.content)}</div>
          <div class="message-time">${time}${statusIcon}</div>
        </div>
      </div>
    `;
  }).join('');

  scrollToBottom();
}

function updateChatHeader() {
  if (!state.activeContact) {
    document.getElementById('chat-title').textContent = '选择一个联系人开始聊天';
    return;
  }
  const c = state.contacts[state.activeContact];
  const status = c && c.online ? ' 🟢在线' : ' ⚫离线';
  document.getElementById('chat-title').textContent = state.activeContact + status;
}

function showSystemMsg(text) {
  const el = document.getElementById('messages-list');
  const div = document.createElement('div');
  div.className = 'system-msg';
  div.textContent = text;
  el.appendChild(div);
  scrollToBottom();
}

function scrollToBottom() {
  requestAnimationFrame(() => {
    const el = document.getElementById('messages-container');
    if (el) el.scrollTop = el.scrollHeight;
  });
}

// ============================================================
// Utilities
// ============================================================
function escHtml(s) {
  const d = document.createElement('div');
  d.textContent = s || '';
  return d.innerHTML;
}
function escAttr(s) {
  return String(s).replace(/"/g, '&quot;').replace(/'/g, '&#39;').replace(/</g, '&lt;');
}
function formatTime(ts) {
  const d = new Date(ts);
  return d.getHours().toString().padStart(2,'0') + ':' + d.getMinutes().toString().padStart(2,'0');
}
function formatDate(ts) {
  const d = new Date(ts), now = new Date();
  if (d.toDateString() === now.toDateString()) return '今天';
  const y = new Date(now); y.setDate(y.getDate() - 1);
  if (d.toDateString() === y.toDateString()) return '昨天';
  return (d.getMonth()+1) + '月' + d.getDate() + '日';
}
function formatContent(t) {
  return escHtml(t).replace(/(https?:\/\/[^\s]+)/g, '<a href="$1" target="_blank">$1</a>');
}
function getStatusIcon(s) {
  if (s === 'sent') return '<span class="message-status sent">✓</span>';
  if (s === 'delivered') return '<span class="message-status delivered">✓✓</span>';
  if (s === 'read') return '<span class="message-status read">✓✓</span>';
  return '';
}

document.addEventListener('DOMContentLoaded', function() {
  document.getElementById('username-input').focus();
});
