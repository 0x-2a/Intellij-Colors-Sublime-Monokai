import java.util.ArrayDeque

class BinaryTree<T>(val data: T) {

    var leftTree: BinaryTree<T>? = null
    var rightTree: BinaryTree<T>? = null

    fun prefix(visitor: (T) -> Unit) {
        visitor(data)
        leftTree?.prefix(visitor)
        rightTree?.prefix(visitor)
    }

    fun infix(visitor: (T) -> Unit) {
        leftTree?.prefix(visitor)
        visitor(data)
        rightTree?.prefix(visitor)
    }

    fun postfix(visitor: (T) -> Unit) {
        leftTree?.prefix(visitor)
        rightTree?.prefix(visitor)
        visitor(data)
    }

    fun visitNodesAtLevel(visitor: (T) -> Unit, targetLevel: Int, currentLevel: Int = 0) {
        if (currentLevel == targetLevel) {
            visitor(data)
        } else {
            leftTree?.visitNodesAtLevel(visitor, targetLevel, currentLevel + 1)
            rightTree?.visitNodesAtLevel(visitor, targetLevel, currentLevel + 1)
        }
    }

    fun height(): Int {
        if (leftTree == null && rightTree == null) {
            return 0
        } else {
            val leftTreeHeight = leftTree?.height() ?: 0
            val rightTreeHeight = rightTree?.height() ?: 0
            return 1 + Math.max(leftTreeHeight, rightTreeHeight)
        }
    }

    fun bfs(visitor: (T) -> Unit) {
        val queue = ArrayDeque<BinaryTree<T>>()
        queue.addLast(this)

        while (!queue.isEmpty()) {
            val node = queue.pollFirst();
            visitor(node.data)

            val left = node.leftTree
            if (left != null) {
                queue.addLast(left)
            }

            val right = node.rightTree
            if (right != null) {
                queue.addLast(right)
            }
        }
    }
}