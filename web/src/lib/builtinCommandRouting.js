import { parseExecutableBuiltinCommand } from './slashCommands.js';

export function builtinCommandRequestForText(text) {
  const builtin = parseExecutableBuiltinCommand(text);
  if (!builtin) return null;
  return {
    command: builtin.name,
    args: builtin.args,
    display_text: builtin.display_text,
  };
}

export function sideQuestionRequestForText(text) {
  const value = String(text || '');
  const match = /^\s*\/(btw|side)\b([\s\S]*)$/i.exec(value);
  if (!match) return null;
  return {
    command: match[1].toLowerCase(),
    question: match[2].trim(),
    display_text: value.trim(),
  };
}

export function turnSteerRequestForText(text) {
  const value = String(text || '');
  const match = /^\s*\/turn\b([\s\S]*)$/i.exec(value);
  if (!match) return null;
  return {
    guidance: match[1].trim(),
    display_text: value.trim(),
  };
}

export function desktopFeedbackRequestForText(text) {
  const value = String(text || '');
  const match = /^\s*\/feedback(?=$|\s)([\s\S]*)$/i.exec(value);
  if (!match) return null;
  return {
    feedbackText: match[1].trim(),
    display_text: value.trim(),
  };
}

export function inputRouteForText(text) {
  const desktopFeedback = desktopFeedbackRequestForText(text);
  if (desktopFeedback) return { kind: 'desktop_feedback', ...desktopFeedback };
  const sideQuestion = sideQuestionRequestForText(text);
  if (sideQuestion) return { kind: 'side_question', ...sideQuestion };
  const turnSteer = turnSteerRequestForText(text);
  if (turnSteer) return { kind: 'turn_steer', ...turnSteer };
  const command = builtinCommandRequestForText(text);
  if (command) return { kind: 'builtin', command };
  return { kind: 'message', text };
}

export function remoteControlSessionRefreshForCommand(command = {}, sessionId = '') {
  const name = String(command?.command || command?.name || '').trim().toLowerCase();
  if (name !== 'rc' && name !== 'remote-control') return null;

  const args = String(command?.args || '').trim().toLowerCase();
  if (args === '') {
    return {
      reason: 'remote-control-bound',
      sessionId: String(sessionId || '').trim(),
    };
  }
  if (args === 'off') {
    return {
      reason: 'remote-control-unbound',
      sessionId: String(sessionId || '').trim(),
    };
  }
  return null;
}

export function sessionCreateOptionsForText(text) {
  if (desktopFeedbackRequestForText(text)
      || sideQuestionRequestForText(text)
      || turnSteerRequestForText(text)) {
    return { auto_start: false };
  }
  const command = builtinCommandRequestForText(text);
  if (command) return { auto_start: false };
  return {
    initial_user_message: text,
    auto_start: true,
  };
}
