// RUN: %target-typecheck-verify-swift %s

// https://github.com/apple/swift/issues/58069

protocol Collection {
  associatedtype SubSequence: Collection
}

protocol BidirectionalCollection: Collection where SubSequence: BidirectionalCollection {}

struct Slice<Base : Collection> : Collection {
  typealias SubSequence = Slice<Base>
}

extension Slice: BidirectionalCollection where Base : BidirectionalCollection {}

protocol SlicedCollection: BidirectionalCollection where SubSequence == Slice<Self> {}
