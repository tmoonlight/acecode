// 内容相同就不触发 React 状态更新。
//
// 背景:Desktop 挂机一两天后渲染进程内存耗尽。侧栏每 5 秒刷新一次,数据没变
// 也会生成新的 Map / Set / 数组写回状态,于是每轮都重新渲染。侧栏组件里几十个
// useCallback 依赖各不相同,它们在不同渲染里交替重建;V8 让同一次渲染里的闭包
// 共享一个作用域,旧回调就把旧渲染的整个作用域(含当时的会话列表)留住,一份
// 接一份连成链,永远回收不掉。实测空闲首页每次渲染约留下 70 KB。
//
// 内容没变时必须连 setState 都不调用,不能只在 updater 里返回旧值:React 18 对
// 「结果与旧值相同」的更新会提前放弃渲染,但 update 对象(连同 updater 闭包和它
// 捕获的数据)仍挂在该 hook 的队列里,要等这个组件下次渲染才清掉。组件一直空闲
// 不渲染时,队列就无限增长 —— 只返回旧值等于把泄漏换了个地方。

import { useRef, useState } from 'react';

const MAX_DEPTH = 32;

const tagOf = (value) => Object.prototype.toString.call(value);

// 原型是某个 realm 的 Object.prototype(或 null)即普通对象;不直接比
// Object.prototype,iframe / vm 里创建的对象原型不同。
function isPlainObject(value) {
  if (value === null || typeof value !== 'object' || tagOf(value) !== '[object Object]') return false;
  const proto = Object.getPrototypeOf(value);
  return proto === null || Object.getPrototypeOf(proto) === null;
}

function equalAt(a, b, depth) {
  if (Object.is(a, b)) return true;
  if (depth > MAX_DEPTH) return false;
  if (a === null || b === null || typeof a !== 'object' || typeof b !== 'object') return false;

  if (Array.isArray(a)) {
    if (!Array.isArray(b) || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i += 1) {
      if (!equalAt(a[i], b[i], depth + 1)) return false;
    }
    return true;
  }
  if (tagOf(a) === '[object Map]') {
    if (tagOf(b) !== '[object Map]' || a.size !== b.size) return false;
    for (const [key, value] of a) {
      if (!b.has(key) || !equalAt(value, b.get(key), depth + 1)) return false;
    }
    return true;
  }
  if (tagOf(a) === '[object Set]') {
    if (tagOf(b) !== '[object Set]' || a.size !== b.size) return false;
    for (const value of a) {
      if (!b.has(value)) return false;
    }
    return true;
  }
  if (isPlainObject(a)) {
    if (!isPlainObject(b)) return false;
    const keys = Object.keys(a);
    if (keys.length !== Object.keys(b).length) return false;
    for (const key of keys) {
      if (!Object.prototype.hasOwnProperty.call(b, key)) return false;
      if (!equalAt(a[key], b[key], depth + 1)) return false;
    }
    return true;
  }
  // Date、类实例、函数等:只认同一引用。
  return false;
}

// 数组 / 普通对象 / Map / Set 按内容比较;Set 成员与 Map 键按引用(侧栏里都是字符串)。
export function structurallyEqual(a, b) {
  return equalAt(a, b, 0);
}

export function keepIfStructurallyEqual(prev, next) {
  return structurallyEqual(prev, next) ? prev : next;
}

// 以 ref 中的最新值为基准计算更新;内容没变就不调用 setState。
// 前提:这份状态的每个写入方都同步更新 ref(否则 ref 可能落后,算出的值会覆盖别人的更新)。
// 返回写入后的最新值。
export function commitIfChanged(ref, setState, updater) {
  const prev = ref.current;
  const next = typeof updater === 'function' ? updater(prev) : updater;
  if (structurallyEqual(prev, next)) return prev;
  ref.current = next;
  setState(next);
  return next;
}

// useState 的替代:setter 身份稳定,内容不变的更新不进 React 队列也不触发渲染。
// 状态只能经这个 setter 写入,内部 ref 因此始终是最新值。
export function useStructuralState(initial) {
  const [value, setValue] = useState(initial);
  const latestRef = useRef(value);
  const [setStructural] = useState(() => (updater) => commitIfChanged(latestRef, setValue, updater));
  return [value, setStructural];
}
