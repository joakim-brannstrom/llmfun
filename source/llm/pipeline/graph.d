module llm.pipeline.graph;

import logger = std.logger;
import std.conv : text;
import std.algorithm : filter, max, map, count;
import std.array : join, appender, empty, array;

interface Node {
    string id() const;
}

struct NodeMeta {
    ulong[] outgoing;
    string output;
    string[] input;
}

interface Edge {
    string fromId() const;
    string toId() const;
    uint maxLoops() const;
    // Called when the graph traverse the edge. Modify the condition if necessary.
    void traverse();
    bool canTraverse(Node from, Node to) const;
    void reset();
}

struct EdgeMeta {
    uint traversalCount;
}

class StopNode : Node {
    string id_;

    this(string id_) {
        this.id_ = id_;
    }

    override string id() const {
        return id_;
    }
}

class Graph {
    private {
        Node startNode_;
        Node stopNode_;
        Node[string] nodes;
        NodeMeta[string] nodesMeta;

        Edge[ulong] edges;
        EdgeMeta[ulong] edgesMeta;

        Node[] queue;
        Node currentPos;

        bool reachedStop;
    }

    void setStartNode(string id) {
        startNode_ = nodes[id];
        currentPos = startNode_;
    }

    Node startNode() @safe pure nothrow @nogc {
        return startNode_;
    }

    void setStopNode(string id) {
        stopNode_ = nodes[id];
    }

    Node stopNode() @safe pure nothrow @nogc {
        return stopNode_;
    }

    void add(Node n) {
        nodes[n.id] = n;
        nodesMeta[n.id] = NodeMeta.init;
    }

    /// Throws if fromId or toId do not exist.
    void add(Edge e) {
        if (e.fromId !in nodes)
            throw new Exception(i"add(Edge): source node '$(e.fromId)' not found".text);
        if (e.toId !in nodes)
            throw new Exception(i"add(Edge): target node '$(e.toId)' not found".text);
        const id = edges.length;
        edges[id] = e;
        edgesMeta[id] = EdgeMeta.init;
    }

    void restart() {
        import std.algorithm : sort;

        currentPos = null;
        reachedStop = false;
        queue = null;
        queue ~= startNode_;
        foreach (ref node; nodesMeta.byKeyValue) {
            node.value = NodeMeta.init;
            foreach (edge; edges.byKeyValue.filter!(a => a.value.fromId == node.key)) {
                node.value.outgoing ~= edge.key;
            }
            node.value.outgoing = node.value.outgoing.sort.array;
        }
        foreach (ref edge; edges.byKeyValue) {
            edge.value.reset;
            edgesMeta[edge.key] = EdgeMeta.init;
        }
    }

    alias Executor = string delegate(Node node, string[] input) nothrow;

    void runNext(Executor executor) {
        if (reachedStop)
            return;
        if (queue.empty) {
            checkEdgeCondition(currentPos);
            return;
        }
        currentPos = queue[0];
        queue = queue[1 .. $];
        if ((cast(StopNode) currentPos) is null) {
            nodesMeta[currentPos.id].output = executor(currentPos, nodesMeta[currentPos.id].input);
        }
        if (currentPos == stopNode_) {
            reachedStop = true;
        } else {
            checkEdgeCondition(currentPos);
        }
    }

    /**
      * Evaluates outgoing edges for a node and traverses the first valid edge.
      * Returns true if an edge was traversed, false otherwise.
      * On success, calls `onReady` with the target node id and the source node.
      */
    private bool _traverseFirstValidEdge(string nodeId, Node fromNode,
            void delegate(string targetId) onReady) {
        foreach (edgeId; nodesMeta[nodeId].outgoing) {
            auto edge = edges[edgeId];
            bool traverse = true;

            if (edge.maxLoops != 0 && edgesMeta[edgeId].traversalCount >= edge.maxLoops) {
                traverse = false;
            }

            if (edge.canTraverse(fromNode, nodes[edge.toId])) {
                edge.traverse();
            } else {
                traverse = false;
            }

            if (traverse) {
                auto targetId = edge.toId;
                edgesMeta[edgeId].traversalCount++;
                nodesMeta[targetId].input ~= nodesMeta[nodeId].output;
                onReady(targetId);
                return true;
            }
        }
        return false;
    }

    /**
     * find the first node that the executing one has outgoing edges to.
     * If conditions are met schedule for execution
     */
    void checkEdgeCondition(Node node) {
        _traverseFirstValidEdge(node.id, node, (string targetId) {
            queue = nodes[targetId] ~ queue;
        });
    }

    /**
     * Called when a node finishes execution. Stores the output, evaluates
     * outgoing edges, and returns the list of nodes that became ready.
     * Preserves single-edge-per-completion behavior (break after first traversal).
     */
    void completeNode(string nodeId, string output, ref Node[] readyNodes) {
        // Store the node's output for edge condition evaluation
        nodesMeta[nodeId].output = output;

        _traverseFirstValidEdge(nodeId, nodes[nodeId], (string targetId) {
            readyNodes ~= nodes[targetId];
        });
    }

    /**
     * Returns the accumulated input strings for a node.
     * Returns null if nodeId does not exist.
     */
    string[] getNodeInputs(string nodeId) const {
        if (nodeId !in nodesMeta)
            return null;
        return nodesMeta[nodeId].input.dup;
    }

    void clearNodeInputs(string nodeId) {
        if (nodeId in nodesMeta)
            nodesMeta[nodeId].input = null;
    }

    bool isDone() @safe pure nothrow const @nogc {
        return reachedStop;
    }

    override string toString() {
        import std.format : formattedWrite;

        auto buf = appender!string();
        buf.put("Graph:\n");
        buf.put("  Nodes:\n");
        foreach (ref n; nodes) {
            string isStartNode = startNode_.id == n.id ? " [start]" : "";
            string isCurrent;
            if (!queue.empty)
                isCurrent = n.id == queue[0].id ? " [here]" : "";
            else if (currentPos)
                isCurrent = n.id == currentPos.id ? " [here]" : "";
            formattedWrite(buf, "    %s%s%s\n", n.id, isStartNode, isCurrent);
        }
        buf.put("  Edges:\n");
        foreach (id, ref e; edges) {
            string loopStr = e.maxLoops > 0 ? i" [loop:$(edgesMeta[id].traversalCount)/$(e.maxLoops)]".text
                : "";
            formattedWrite(buf, "    %s -> %s [condition:%s]%s\n", e.fromId,
                    e.toId, e.canTraverse(nodes[e.fromId], nodes[e.toId]), loopStr);
        }
        if (!queue.empty) {
            buf.put("  Queue:\n");
            foreach (ref n; queue) {
                formattedWrite(buf, "    %s\n", n.id);
            }
        }
        return buf.data;
    }
}

version (unittest) {
    class DummyNode : Node {
        string id_;

        this(string id_) {
            this.id_ = id_;
        }

        override string id() const {
            return id_;
        }

        override string toString() {
            return i"DummyNode($(id_))".text;
        }
    }

    class DummyEdge : Edge {
        string fromId_;
        string toId_;
        uint maxLoops_;

        this(string fromId_, string toId_, uint maxLoops_ = 0) {
            this.fromId_ = fromId_;
            this.toId_ = toId_;
            this.maxLoops_ = maxLoops_;
        }

        override string fromId() const {
            return fromId_;
        }

        override string toId() const {
            return toId_;
        }

        override uint maxLoops() const {
            return maxLoops_;
        }

        override bool canTraverse(Node from, Node to) const {
            return true;
        }

        override void traverse() {
        }

        override void reset() {
        }
    }

    import std.stdio : writeln, writefln;
}

// test the simplest node traversal
unittest {
    auto g = new Graph();
    g.add(new DummyNode("a"));
    g.add(new DummyNode("b"));
    g.add(new DummyEdge("a", "b", 0));
    g.setStartNode("a");
    g.setStopNode("b");

    g.restart;
    assert(g.queue[0].id == "a");
    assert(!g.isDone);

    int executCount;
    string executor(Node n, string[] input) nothrow {
        executCount++;
        return null;
    }

    g.runNext(&executor);
    assert(!g.isDone);
    g.runNext(&executor);
    assert(g.isDone);
    assert(executCount == 2);
}

// a node that is never executed, missing edge to it
unittest {
    auto g = new Graph();
    g.add(new DummyNode("a"));
    g.add(new DummyNode("b"));
    g.add(new DummyNode("c"));
    g.add(new DummyEdge("a", "c", 0));
    g.add(new DummyEdge("b", "c", 0));
    g.setStartNode("a");
    g.setStopNode("c");

    g.restart;
    assert(g.queue[0].id == "a");
    assert(!g.isDone);

    string[] nodes;
    string executor(Node n, string[] input) nothrow {
        try {
            nodes ~= n.id;
        } catch (Exception e) {
        }
        return null;
    }

    g.runNext(&executor);
    assert(!g.isDone);
    g.runNext(&executor);
    assert(g.isDone);
    assert(nodes == ["a", "c"]);

    g.restart;
    assert(!g.isDone);
}

// a conditional node
unittest {
    class DummyCondEdge : DummyEdge {
        this(string fromId_, string toId_, uint maxLoops_ = 0, int* cnt) {
            super(fromId_, toId_, maxLoops_);
            this.cnt = cnt;
        }

        int* cnt;
        override bool canTraverse(Node from, Node to) const {
            return *cnt > 1;
        }

        override void traverse() {
            ++(*cnt);
        }

        override void reset() {
            *cnt = 0;
        }

        override string toString() @safe const {
            return "DummyCondEdge";
        }
    }

    int cnt;
    auto g = new Graph();
    g.add(new DummyNode("a"));
    g.add(new DummyNode("b"));
    g.add(new DummyNode("c"));
    g.add(new DummyEdge("a", "b", 0));
    g.add(new DummyCondEdge("b", "c", 0, &cnt));
    g.add(new DummyEdge("b", "a", 2));
    g.setStartNode("a");
    g.setStopNode("c");

    g.restart;
    assert(g.queue[0].id == "a");
    assert(!g.isDone);

    string[] nodes;
    string executor(Node n, string[] input) nothrow {
        try {
            nodes ~= n.id;
        } catch (Exception e) {
        }
        return null;
    }

    for (int i = 0; i < 8; ++i) { // run a,b until maxLoop. Get stuck on b
        g.runNext(&executor);
        assert(!g.isDone);
    }
    cnt = 2;
    g.runNext(&executor); // b traverse to c
    assert(!g.isDone);
    g.runNext(&executor); // execute c
    assert(g.isDone);
    assert(nodes == ["a", "b", "a", "b", "a", "b", "c"]);

    g.restart;
    assert(!g.isDone);
}

unittest {
    auto g = new Graph();
    g.add(new DummyNode("a"));
    g.add(new DummyNode("b"));
    g.add(new DummyNode("c"));
    g.add(new DummyEdge("a", "b", 0));
    g.add(new DummyEdge("b", "a", 3));
    g.add(new DummyEdge("b", "c", 0));
    g.setStartNode("a");
    g.setStopNode("c");

    g.restart;
    assert(g.queue[0].id == "a");
    assert(!g.isDone);

    string[] nodes;
    string[][] inputs;
    string executor(Node n, string[] input) nothrow {
        try {
            nodes ~= n.id;
            inputs ~= input;
            return n.id;
        } catch (Exception e) {
        }
        return null;
    }

    foreach (i; 0 .. 8) {
        g.runNext(&executor);
        assert(!g.isDone);
    }

    g.runNext(&executor);
    assert(g.isDone);
    assert(nodes == ["a", "b", "a", "b", "a", "b", "a", "b", "c"]);
    assert(inputs[$ - 1] == ["b"]); // node c had only b as input
    assert(inputs[$ - 2] == ["a", "a", "a", "a"]); // node b had multiple a as input because of the loop
}

unittest {
    auto g = new Graph();
    g.add(new DummyNode("a"));
    g.add(new DummyNode("b"));
    g.add(new DummyNode("c"));
    g.add(new DummyEdge("a", "b", 1));
    g.add(new DummyEdge("b", "a", 0));
    g.add(new DummyEdge("a", "c", 0));
    g.setStartNode("a");
    g.setStopNode("c");

    string[] nodes;
    string[][] inputs;
    string executor(Node n, string[] input) nothrow {
        try {
            nodes ~= n.id;
            inputs ~= input;
            return n.id;
        } catch (Exception e) {
        }
        return null;
    }

    g.restart;
    foreach (i; 0 .. 10) {
        g.runNext(&executor);
    }
}

// Test that restart() resets currentPos to null
unittest {
    auto g = new Graph();
    g.add(new DummyNode("a"));
    g.add(new DummyNode("b"));
    g.add(new DummyEdge("a", "b", 0));
    g.setStartNode("a");
    g.setStopNode("b");

    g.restart;
    assert(g.currentPos is null);

    string executor(Node n, string[] input) nothrow {
        return null;
    }

    g.runNext(&executor);
    // After runNext, currentPos should be the dequeued node, not null
    assert(!(g.currentPos is null));
    assert(g.currentPos.id == "a");

    g.restart;
    // After restart, currentPos should be null again
    assert(g.currentPos is null);
}
