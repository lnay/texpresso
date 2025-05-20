cd "$(dirname "$0")"

echo "Close each window after confirming the content matches the description here:"
echo "."
echo "."

echo "1/4. LaTeX document with some text and an equation, clicking on document make synctex sexpressions appear below:"
echo
../build/texpresso simple.tex |& sed -n '/^(synctex/p'

echo
echo "2/4. One sentence of virtual text"
../build/texpresso simple.tex <<scm &> /dev/null
(open "`pwd`/simple.tex" "\\\\documentclass[12pt]{article}\n\n\\\\begin{document}\n\nVirtual file content\n\n\\\\end{document}\n")
scm


echo
echo "3/4. One edited sentence"
../build/texpresso simple.tex <<scm &> /dev/null
(open "`pwd`/simple.tex" "\\\\documentclass[12pt]{article}\n\n\\\\begin{document}\n\nVirtual file content\n\n\\\\end{document}\n")
(change-lines "`pwd`/simple.tex" 4 1 "Edited virtual file content")
scm

echo
echo "4/4. A forward search to the second page"
../build/texpresso simple.tex <<scm &> /dev/null
(open "`pwd`/simple.tex" "\\\\documentclass[12pt]{article}\n\n\\\\begin{document}\n\nVirtual file content\n\\\\clearpage\nSecond page\n\n\\\\end{document}\n")
(synctex-forward "`pwd`/simple.tex" 3)
(synctex-forward "`pwd`/simple.tex" 7)
scm
