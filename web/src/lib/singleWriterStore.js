// 单写者状态 store。
//
// 存在理由:实时状态曾经同时被两个入口写入 —— 异步回调(WebSocket 事件、REST
// 回调、定时器)同步写一个可变引用,React 的被动 effect 又把某次渲染保存的旧
// 快照写回同一个引用。被动 effect 的刷新时机与 WebSocket 消息在事件循环里互相穿插,一旦
// 回写发生在“提交之后、下一次提交之前”,其后到达的增量就会从旧快照继续拼接,
// 中间已经提交的变更被永久覆盖。这条路径在传输层完全不可见:事件序号仍然单调
// 增长,去重逻辑不会报警,服务端数据是完整的,只有页面内容随机缺片段。
//
// 当前消费者:transcript 实时状态(sessionTranscript.js)与聊天排队输入状态
// (ChatView.jsx)。两者原本都是同一种双写结构。
//
// 因此这里把状态所有权移出 React:
// - `commit` 是唯一写入口,且只接受 producer 函数,入参恒为最新已提交状态。
//   值形式的写入天然允许调用方传入过期快照,producer 形式在入口就消灭了它。
// - `subscribe` 的通知不携带状态,订阅者必须重新 `getState()`,所以订阅者手里
//   永远不会出现“一份可以拿去回写的旧状态”。
export function createSingleWriterStore(initialState) {
    let state = initialState;
    let revision = 0;
    let notifying = false;
    let pendingNotification = false;
    const listeners = new Set();

    function notify() {
        // 订阅者可能在通知里再次 commit(例如 self-heal 覆写)。此时不重入广播,
        // 只置位标记,由最外层通知循环继续广播,保证嵌套提交既不丢通知也不会
        // 让同一份状态被递归重复广播。
        if (notifying) {
            pendingNotification = true;
            return;
        }
        notifying = true;
        try {
            do {
                pendingNotification = false;
                for (const listener of Array.from(listeners)) {
                    listener();
                }
            } while (pendingNotification);
        } finally {
            notifying = false;
            pendingNotification = false;
        }
    }

    return {
        getState() {
            return state;
        },
        // 状态版本号,单调递增。供诊断与测试断言“这次提交是否真的产生了新状态”。
        getRevision() {
            return revision;
        },
        // 唯一写入口。producer 必须是函数;返回同一对象或空值表示“无变化”,
        // 不升版本、不通知订阅者。
        commit(producer) {
            if (typeof producer !== 'function') {
                throw new TypeError('single-writer store commit requires a producer function');
            }
            const next = producer(state);
            if (!next || next === state) return state;
            state = next;
            revision += 1;
            notify();
            return state;
        },
        subscribe(listener) {
            if (typeof listener !== 'function') return () => {};
            listeners.add(listener);
            return () => { listeners.delete(listener); };
        },
    };
}
