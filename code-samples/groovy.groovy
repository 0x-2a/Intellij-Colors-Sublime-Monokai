File jsDoc = new File("asdf.js")
jsDoc.delete()

//Write the intro to index document
jsDoc << '//     This high-level directory exposes the internal structure \n'

doccoClosure = {
    //Filter out svn and lib folders
    if (!it.canonicalPath.toString().contains(".svn") && !it.canonicalPath.toString().contains("lib")) {

        //recurse through each directory
        it.eachDir(doccoClosure);

        strTemp = it.canonicalPath.toString()

        //Add the directory to index document
        jsDoc << "//# `${dirPath}` \n";

        //Create folders for documentation
        newDirName = dirPath.substring(dirPath.lastIndexOf("/") + 1, dirPath.length())
        new File("${newDirName}").mkdir()

        it.eachFile {
            //Only js files, not json
            if (it.canonicalPath.toString().contains(".js") && !it.canonicalPath.toString().contains(".json")) {
                //Get the path/filename
                strTempB = it.canonicalPath.toString();
                jsFullName = strTempB.substring(strTempB.lastIndexOf("/") + 1, strTempB.length())
                jsName = jsFullName.substring(0,jsFullName.indexOf(".js"))

                //Document the file
                def command = "docco ${it.canonicalPath}"
                def proc = command.execute()
                proc.waitFor()

                //Setup docco structure around the file
                File jsFile = new File("docs/${jsName}.html")
                File dir = new File("${newDirName}");
                File doccoCSSFile = new File("docs/docco.css")

                //Make docco structure specific
                jsFile.renameTo(new File(dir,jsFile.getName()));
                doccoCSSFile.renameTo(new File(dir,doccoCSSFile.getName()));

                println("Documenting ${jsFullName} to lb-docs/${newDirName}/${jsFile.getName()}")

                //Add the document and path to the index document
                jsDoc << "// [${jsFullName}](${newDirName}\\${jsFile.getName()})   \n"

            }
        }
    }
}


def procC = commandC.execute()
procC.waitFor()

//Wrap with docco structure
File doccoCSSFile = new File("docs/docco.css")
File dir = new File(".");
jsFile.renameTo(new File(dir,jsFile.getName()));
doccoCSSFile.renameTo(new File(dir,doccoCSSFile.getName()));

//Open index and done.
def procFinal = commandFinal.execute()
procFinal.waitFor()