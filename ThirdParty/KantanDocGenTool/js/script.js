function Init()
{
	var coll = document.getElementsByClassName("collapsible");
	var i;
	//alert(coll.length);
	for (i = 0; i < coll.length; i++) {
	coll[i].addEventListener("click", function() {
		this.classList.toggle("active");
		var content = this.nextElementSibling;
		//if (content.style.maxHeight!="0px"){
		//content.style.maxHeight = "0px";
		//} else {
		//content.style.maxHeight = content.scrollHeight + "px";
		//} 	

		if (content.style.visibility!="collapse"){
		content.style.visibility = "collapse";
		} else {
		content.style.visibility = "visible";
		} 
	});
	}
}