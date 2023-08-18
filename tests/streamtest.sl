(def baseStr "test ")
(def s (make-str-ostream baseStr))

(assert (= "test " (stream-get-str s)))
(write! s "hello")
(assert (= "test hello" (stream-get-str s)))
(assert !(file? s))
(def inS (make-str-istream (stream-get-str s)))
(assert (= "test" (read! inS 4)))
(assert (= " hello" (read! inS)))
(assert (eof? (read! inS)))
(assert (eof? (read! inS -2)))
(assert (eof? (read! inS 100)))
(assert !(file? inS))
(def emptyS (make-str-istream ""))
(assert (eof? (read! emptyS 1)))
(assert (eof? (read-byte! emptyS)))
(write-byte! s 70)
(seek-off! s -10)
(write! s "wow!")
(assert (= "twow!helloF" (stream-get-str s)))
(assert (= 5 (tell s)))

;-(def fs (make-file-istream "testfile.txt"))
(assert (file? fs))
(assert (file-open? fs))
(read! fs)
(seek-off! fs -10)
(read! fs)-;

; input-from! testing
(def s "Hello, World!\nSecond line")
(def s2 "Hello, World!\nSecond line\n")

(def inS (make-str-istream s))
(assert (= "Hello, World!" (input-from! inS)))
(assert (= "Second line" (input-from! inS)))
(assert (eof? (input-from! inS)))
(assert (eof? (input-from! inS)))

(def inS (make-str-istream s2))
(assert (= "Hello, World!" (input-from! inS)))
(assert (= "Second line" (input-from! inS)))
(assert (eof? (input-from! inS)))
(assert (eof? (input-from! inS)))

(def numStr "My number: ")
(def outS (make-str-ostream numStr))
(output-to! outS 37 "test")
(output-to! outS 'testSym " " 'def)
(assert (= "My number: 37testtestSym def" numStr))


(output "stream passed\n")
