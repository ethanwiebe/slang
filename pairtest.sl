(pair (pair 1 2) 2)

(def first (pair (pair 1 2) 3))
(setL! (L first) 100)

(def a (pair 1 2))

(def sec (pair a a))

(setL! sec 5)
sec
a
