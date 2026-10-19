// 身份永远不变的回调:调用时转发到最近一次提交的渲染里的实现。
//
// 背景:侧栏几十个 useCallback 依赖各不相同,在不同渲染里交替重建。V8 让同一次
// 渲染里的闭包共享一个作用域,于是第 N 次渲染的作用域里保存着第 K 次渲染创建的
// 回调,它又保存着第 J 次的……旧渲染的作用域(含当时的整份会话列表)一份接一份
// 连成链,随渲染次数无限增长 —— Desktop 挂机一两天后渲染进程内存耗尽。
//
// 这里返回的包装函数只在本 hook 第一次调用时创建,只捕获 ref;组件作用域里不再
// 持有旧渲染创建的闭包,链条就断了。ref 只保存最新一次渲染的实现。
//
// 适用:事件处理、effect 里调用的动作。不要用在渲染期间调用的函数上(渲染时拿到
// 的是上一次提交的实现),也不要用在需要靠身份变化触发 effect 重跑的地方。

import { useInsertionEffect, useRef, useState } from 'react';

export function useStableCallback(implementation) {
  const latest = useRef(implementation);
  // insertion effect 在整棵树的 layout effect 之前执行,子组件的 layout effect
  // 调用它时也能拿到本次提交的实现。
  useInsertionEffect(() => {
    latest.current = implementation;
  });
  const [stable] = useState(() => function stableCallback(...args) {
    return latest.current.apply(this, args);
  });
  return stable;
}
